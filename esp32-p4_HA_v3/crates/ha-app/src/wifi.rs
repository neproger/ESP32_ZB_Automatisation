//! Wi-Fi service: the only owner of radio calls.
//!
//! Commands arrive through Domain.post or the display navigation queue; the
//! service task talks to the C radio, publishes canonical WIFI_STATUS/SCAN
//! records for Display/Web consumers.

use std::ffi::{c_char, CStr, CString};
use std::sync::mpsc::{sync_channel, Receiver, RecvTimeoutError, SyncSender};
use std::sync::Mutex;
use std::time::Duration;

use ha_domain::canonical::{
    WifiKnownRecord, WifiScanRecord, WifiStatusRecord, ENTITY_WIFI_KNOWN, ENTITY_WIFI_SCAN,
    ENTITY_WIFI_STATUS, SOURCE_WIFI, WIFI_DEVICE_UID, WIFI_KNOWN_MAX, WIFI_PASSWORD_MAX,
    WIFI_SCAN_MAX, WIFI_SSID_MAX,
};
use ha_domain::{DomainHandle, Fact, FactMeta, Filter, Value};

enum Command {
    Scan,
    Connect { ssid: String, password: String },
}

static COMMANDS: Mutex<Option<SyncSender<Command>>> = Mutex::new(None);

extern "C" {
    fn ha_wifi_start() -> i32;
    fn ha_wifi_ip(out: *mut c_char, len: usize) -> i32;
    fn ha_wifi_scan_start() -> i32;
    fn ha_wifi_scan_ready() -> i32;
    fn ha_wifi_scan_clear();
    fn ha_wifi_scan_count() -> i32;
    fn ha_wifi_scan_get(index: i32, ssid: *mut c_char, len: usize, rssi: *mut i32) -> i32;
    fn ha_wifi_status(ssid: *mut c_char, len: usize, rssi: *mut i32) -> i32;
    fn ha_wifi_connect_ssid(ssid: *const c_char, password: *const c_char) -> i32;
    fn ha_wifi_known_add(ssid: *const c_char, password: *const c_char) -> i32;
    fn ha_wifi_known_remove(ssid: *const c_char) -> i32;
    fn ha_wifi_known_count() -> i32;
    fn ha_wifi_known_get(
        index: i32,
        ssid: *mut c_char,
        len: usize,
        password: *mut c_char,
        password_len: usize,
    ) -> i32;
    fn ha_time_start();
}

pub fn start(domain: DomainHandle) {
    let (tx, rx) = sync_channel(4);
    let (known_tx, known_rx) = sync_channel(64);
    if let Err(error) = domain.subscribe_channel(Filter::entity(ENTITY_WIFI_KNOWN), known_tx) {
        log::error!("wifi: known subscription failed: {error:?}");
        return;
    }
    *COMMANDS.lock().unwrap() = Some(tx);
    std::thread::Builder::new()
        .name("wifi".into())
        .stack_size(8192)
        .spawn(move || run(domain, rx, known_rx))
        .expect("wifi thread");
}

pub fn request_scan() -> Result<(), ha_domain::Error> {
    send(Command::Scan)
}

pub fn request_connect(ssid: String, password: String) -> Result<(), ha_domain::Error> {
    send(Command::Connect { ssid, password })
}

fn send(command: Command) -> Result<(), ha_domain::Error> {
    let sender = COMMANDS.lock().unwrap();
    let Some(sender) = sender.as_ref() else {
        return Err(ha_domain::Error::InvalidState);
    };
    sender
        .try_send(command)
        .map_err(|_| ha_domain::Error::NoSpace)
}

/// `ha_wifi_connect_args_t` (96 bytes): ssid[32], password[64].
pub fn encode_connect(ssid: &str, password: &str) -> [u8; 96] {
    let mut out = [0u8; 96];
    let ssid = ssid.as_bytes();
    let password = password.as_bytes();
    out[..ssid.len().min(WIFI_SSID_MAX)].copy_from_slice(&ssid[..ssid.len().min(WIFI_SSID_MAX)]);
    out[WIFI_SSID_MAX..WIFI_SSID_MAX + password.len().min(WIFI_PASSWORD_MAX)]
        .copy_from_slice(&password[..password.len().min(WIFI_PASSWORD_MAX)]);
    out
}

pub fn decode_connect(args: &[u8]) -> Option<(String, String)> {
    if args.len() != 96 {
        return None;
    }
    let ssid = CStr::from_bytes_until_nul(&args[..WIFI_SSID_MAX])
        .ok()?
        .to_string_lossy()
        .into_owned();
    let password = CStr::from_bytes_until_nul(&args[WIFI_SSID_MAX..])
        .ok()?
        .to_string_lossy()
        .into_owned();
    Some((ssid, password))
}

pub fn connect_from_args(args: &[u8]) -> Result<(), ha_domain::Error> {
    let (ssid, password) = decode_connect(args).ok_or(ha_domain::Error::InvalidSize)?;
    if ssid.is_empty() {
        return Err(ha_domain::Error::InvalidArg);
    }
    request_connect(ssid, password)
}

fn known_records(domain: &DomainHandle) -> Result<Vec<(u32, WifiKnownRecord)>, ha_domain::Error> {
    let snapshot = domain.entity_snapshot(ENTITY_WIFI_KNOWN)?;
    let mut records = Vec::new();
    for index in 0..snapshot.len() {
        let Some(key) = snapshot.key(index) else {
            continue;
        };
        let Some(record) = snapshot.record(index) else {
            continue;
        };
        if key.len() == 4 && record.len() == 96 {
            let id = u32::from_le_bytes(key.try_into().expect("known key"));
            if let Some(record) = WifiKnownRecord::decode(record) {
                records.push((id, record));
            }
        }
    }
    Ok(records)
}

fn text(bytes: &[u8]) -> String {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

fn add_radio_known(ssid: &str, password: &str) {
    let (Ok(ssid), Ok(password)) = (CString::new(ssid), CString::new(password)) else {
        return;
    };
    unsafe { ha_wifi_known_add(ssid.as_ptr(), password.as_ptr()) };
}

fn remember_known(domain: &DomainHandle, ssid: &str, password: &str) {
    let records = match known_records(domain) {
        Ok(records) => records,
        Err(error) => {
            log::error!("wifi: known snapshot failed: {error:?}");
            return;
        }
    };
    let existing = records
        .iter()
        .position(|(_, record)| text(&record.ssid) == ssid);
    let id = match existing {
        Some(index) => records[index].0,
        None => match (0..WIFI_KNOWN_MAX as u32)
            .find(|id| !records.iter().any(|(used, _)| used == id))
        {
            Some(id) => id,
            None => {
                log::warn!("wifi: known AP list full, \"{ssid}\" not saved");
                return;
            }
        },
    };
    let mut record = WifiKnownRecord::default();
    let ssid_bytes = ssid.as_bytes();
    let password_bytes = password.as_bytes();
    record.ssid[..ssid_bytes.len().min(WIFI_SSID_MAX)]
        .copy_from_slice(&ssid_bytes[..ssid_bytes.len().min(WIFI_SSID_MAX)]);
    record.password[..password_bytes.len().min(WIFI_PASSWORD_MAX)]
        .copy_from_slice(&password_bytes[..password_bytes.len().min(WIFI_PASSWORD_MAX)]);
    let key = id.to_le_bytes();
    let encoded = record.encode();
    let meta = FactMeta {
        source: SOURCE_WIFI as u8,
        value: Value::NONE,
    };
    if let Err(error) = domain.entity_put(ENTITY_WIFI_KNOWN, &key, &encoded, meta) {
        log::error!("wifi: known save failed for \"{ssid}\": {error:?}");
        return;
    }
    add_radio_known(ssid, password);
}

fn load_domain_known(domain: &DomainHandle) {
    let records = match known_records(domain) {
        Ok(records) => records,
        Err(error) => {
            log::error!("wifi: known snapshot failed: {error:?}");
            return;
        }
    };
    for (_, record) in records {
        add_radio_known(&text(&record.ssid), &text(&record.password));
    }
}

fn resync_known(domain: &DomainHandle) {
    let records = match known_records(domain) {
        Ok(records) => records,
        Err(error) => {
            log::error!("wifi: known resync snapshot failed: {error:?}");
            return;
        }
    };

    let count = unsafe { ha_wifi_known_count() };
    for index in (0..count).rev() {
        let mut ssid = [0 as c_char; 33];
        if unsafe {
            ha_wifi_known_get(
                index,
                ssid.as_mut_ptr(),
                ssid.len(),
                std::ptr::null_mut(),
                0,
            )
        } != 0
        {
            unsafe { ha_wifi_known_remove(ssid.as_ptr()) };
        }
    }
    for (_, record) in records {
        add_radio_known(&text(&record.ssid), &text(&record.password));
    }
}

fn drain_known(domain: &DomainHandle, events: &Receiver<Fact>) {
    let mut changed = false;
    while events.try_recv().is_ok() {
        changed = true;
    }
    if changed {
        resync_known(domain);
    }
}

fn run(domain: DomainHandle, commands: Receiver<Command>, known_events: Receiver<Fact>) {
    load_domain_known(&domain);
    let err = unsafe { ha_wifi_start() };
    if err != 0 {
        log::error!("wifi start failed: {err}");
        return;
    }
    let mut last_ip: Option<String> = None;
    let mut last_poll = 0u64;
    let mut last_scan_poll = 0u64;
    loop {
        match commands.recv_timeout(Duration::from_millis(500)) {
            Ok(Command::Scan) => {
                if unsafe { ha_wifi_scan_start() } != 0 {
                    log::warn!("wifi: scan request rejected");
                }
            }
            Ok(Command::Connect { ssid, password }) => {
                let (Ok(ssid_c), Ok(password_c)) =
                    (CString::new(ssid.as_str()), CString::new(password.as_str()))
                else {
                    continue;
                };
                let rc = unsafe { ha_wifi_connect_ssid(ssid_c.as_ptr(), password_c.as_ptr()) };
                if rc != 0 {
                    log::warn!("wifi: connect request rejected ({rc})");
                } else {
                    log::info!("wifi: connect requested");
                    remember_known(&domain, &ssid, &password);
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
            Err(RecvTimeoutError::Timeout) => {}
        }
        drain_known(&domain, &known_events);

        let now = now_ms();
        if now.saturating_sub(last_poll) >= 1000 {
            last_poll = now;
            let mut buf = [0 as c_char; 32];
            if unsafe { ha_wifi_ip(buf.as_mut_ptr(), buf.len()) } != 0 {
                let ip = unsafe { CStr::from_ptr(buf.as_ptr()) }
                    .to_string_lossy()
                    .into_owned();
                if last_ip.as_deref() != Some(ip.as_str()) {
                    log::info!("wifi ip={ip}");
                    unsafe { ha_time_start() };
                    log::info!("time: SNTP started (UTC)");
                    last_ip = Some(ip);
                }
            }
            publish_status(&domain);
        }

        if now.saturating_sub(last_scan_poll) >= 500 {
            last_scan_poll = now;
            if unsafe { ha_wifi_scan_ready() } != 0 {
                let results = collect_scan();
                log::info!("wifi: scan result {} networks", results.len());
                publish_scan(&domain, &results);
                unsafe { ha_wifi_scan_clear() };
            }
        }
    }
}

fn collect_scan() -> Vec<(String, i8)> {
    let count = unsafe { ha_wifi_scan_count() };
    let mut results = Vec::new();
    for index in 0..count {
        let mut ssid = [0 as c_char; 33];
        let mut rssi = 0i32;
        if unsafe { ha_wifi_scan_get(index, ssid.as_mut_ptr(), ssid.len(), &mut rssi) } != 0 {
            let ssid = unsafe { CStr::from_ptr(ssid.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            if !ssid.is_empty() {
                results.push((ssid, rssi as i8));
            }
        }
    }
    results
}

fn publish_status(domain: &DomainHandle) {
    let mut ssid = [0 as c_char; 33];
    let mut rssi = 0i32;
    let connected = unsafe { ha_wifi_status(ssid.as_mut_ptr(), ssid.len(), &mut rssi) } != 0;
    let mut name = [0u8; WIFI_SSID_MAX];
    if connected {
        let bytes = unsafe { CStr::from_ptr(ssid.as_ptr()) }.to_bytes();
        name[..bytes.len().min(WIFI_SSID_MAX)]
            .copy_from_slice(&bytes[..bytes.len().min(WIFI_SSID_MAX)]);
    }
    let record = WifiStatusRecord {
        state: if connected { 3 } else { 0 },
        connected: connected as u8,
        rssi: rssi as i8,
        ssid: name,
    }
    .encode();
    let meta = FactMeta {
        source: SOURCE_WIFI as u8,
        value: Value::NONE,
    };
    let key = WIFI_DEVICE_UID.to_le_bytes();
    match domain.entity_put(ENTITY_WIFI_STATUS, &key, &record, meta) {
        Ok(true) => log::info!(
            "wifi: status state={} connected={} rssi={}",
            record[0],
            record[1],
            record[2] as i8
        ),
        Ok(false) => {}
        Err(error) => log::error!("wifi: status write failed: {error:?}"),
    }
}

fn publish_scan(domain: &DomainHandle, results: &[(String, i8)]) {
    let mut record = WifiScanRecord::default();
    for (index, (ssid, rssi)) in results.iter().take(WIFI_SCAN_MAX).enumerate() {
        let bytes = ssid.as_bytes();
        record.aps[index].ssid[..bytes.len().min(WIFI_SSID_MAX)]
            .copy_from_slice(&bytes[..bytes.len().min(WIFI_SSID_MAX)]);
        record.aps[index].rssi = *rssi;
        record.count += 1;
    }
    let meta = FactMeta {
        source: SOURCE_WIFI as u8,
        value: Value::NONE,
    };
    let key = WIFI_DEVICE_UID.to_le_bytes();
    if let Err(error) = domain.entity_put(ENTITY_WIFI_SCAN, &key, &record.encode(), meta) {
        log::error!("wifi: scan write failed: {error:?}");
    }
}

fn now_ms() -> u64 {
    (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000).max(0) as u64
}
