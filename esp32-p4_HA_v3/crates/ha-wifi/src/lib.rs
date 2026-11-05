//! Wi-Fi service: the only owner of radio calls.
//!
//! Commands arrive through Domain.post or the display navigation queue; the
//! service task talks to the C radio, publishes canonical WIFI_STATUS/SCAN
//! records for Display/Web consumers.

use std::ffi::{c_char, CStr, CString};
use std::sync::mpsc::{sync_channel, Receiver, RecvTimeoutError, SyncSender};
use std::sync::Mutex;
use std::time::{Duration, Instant};

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
    fn ha_wifi_scan_start() -> i32;
    fn ha_wifi_scan_ready() -> i32;
    fn ha_wifi_scan_clear();
    fn ha_wifi_scan_count() -> i32;
    fn ha_wifi_scan_get(
        index: i32,
        ssid: *mut c_char,
        len: usize,
        rssi: *mut i32,
        auth: *mut i32,
        channel: *mut i32,
    ) -> i32;
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

fn next_known_id(records: &[(u32, WifiKnownRecord)]) -> Option<u32> {
    if records.len() >= WIFI_KNOWN_MAX {
        return None;
    }
    Some(records.iter().map(|(id, _)| *id).max().unwrap_or(0) + 1)
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
        None => match next_known_id(&records) {
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
    let mut last_poll = Instant::now();
    let mut last_scan_poll = last_poll;
    let mut state = 0u8;
    let mut last_connect = Instant::now();
    let mut was_connected = false;
    loop {
        match commands.recv_timeout(Duration::from_millis(500)) {
            Ok(Command::Scan) => {
                state = 1;
                let connected = publish_status(&domain, state);
                if unsafe { ha_wifi_scan_start() } != 0 {
                    log::warn!("wifi: scan request rejected");
                    state = if connected { 3 } else { 0 };
                    publish_status(&domain, state);
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
                    state = 4;
                    publish_status(&domain, state);
                } else {
                    log::info!("wifi: connect requested");
                    state = 2;
                    last_connect = Instant::now();
                    publish_status(&domain, state);
                    remember_known(&domain, &ssid, &password);
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
            Err(RecvTimeoutError::Timeout) => {}
        }
        drain_known(&domain, &known_events);

        if last_poll.elapsed() >= Duration::from_secs(1) {
            last_poll = Instant::now();
            let connected = publish_status(&domain, state);
            if connected && !was_connected {
                clear_scan(&domain);
            }
            was_connected = connected;
            state = next_status_state(
                state,
                connected,
                last_connect.elapsed() > Duration::from_secs(15),
            );
        }

        if last_scan_poll.elapsed() >= Duration::from_millis(500) {
            last_scan_poll = Instant::now();
            if unsafe { ha_wifi_scan_ready() } != 0 {
                let results = collect_scan();
                log::info!("wifi: scan result {} networks", results.len());
                publish_scan(&domain, &results);
                unsafe { ha_wifi_scan_clear() };
                let connected = publish_status(&domain, 3);
                state = if connected { 3 } else { 0 };
            }
        }
    }
}

struct ScanAp {
    ssid: String,
    rssi: i8,
    auth: u8,
}

fn collect_scan() -> Vec<ScanAp> {
    let count = unsafe { ha_wifi_scan_count() };
    let mut results = Vec::new();
    for index in 0..count {
        let mut ssid = [0 as c_char; 33];
        let mut rssi = 0i32;
        let mut auth = 0i32;
        let mut channel = 0i32;
        if unsafe {
            ha_wifi_scan_get(
                index,
                ssid.as_mut_ptr(),
                ssid.len(),
                &mut rssi,
                &mut auth,
                &mut channel,
            )
        } != 0
        {
            let ssid = unsafe { CStr::from_ptr(ssid.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            if !ssid.is_empty() {
                results.push(ScanAp {
                    ssid,
                    rssi: rssi as i8,
                    auth: auth.clamp(0, 4) as u8,
                });
            }
        }
    }
    results
}

fn next_status_state(state: u8, connected: bool, connect_timed_out: bool) -> u8 {
    if connected {
        if state == 1 {
            1
        } else {
            3
        }
    } else if state == 2 {
        if connect_timed_out {
            4
        } else {
            2
        }
    } else if state == 1 {
        1
    } else {
        0
    }
}
fn publish_status(domain: &DomainHandle, state: u8) -> bool {
    let mut ssid = [0 as c_char; 33];
    let mut rssi = 0i32;
    let connected = unsafe { ha_wifi_status(ssid.as_mut_ptr(), ssid.len(), &mut rssi) } != 0;
    let mut name = [0u8; WIFI_SSID_MAX];
    if connected {
        let bytes = unsafe { CStr::from_ptr(ssid.as_ptr()) }.to_bytes();
        name[..bytes.len().min(WIFI_SSID_MAX)]
            .copy_from_slice(&bytes[..bytes.len().min(WIFI_SSID_MAX)]);
    }
    let state = if connected && state == 2 {
        3
    } else if !connected && state == 3 {
        0
    } else {
        state
    };
    let record = WifiStatusRecord {
        state,
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
    connected
}

fn scan_record(results: &[ScanAp]) -> WifiScanRecord {
    let mut record = WifiScanRecord::default();
    for (index, ap) in results.iter().take(WIFI_SCAN_MAX).enumerate() {
        let bytes = ap.ssid.as_bytes();
        record.aps[index].ssid[..bytes.len().min(WIFI_SSID_MAX)]
            .copy_from_slice(&bytes[..bytes.len().min(WIFI_SSID_MAX)]);
        record.aps[index].rssi = ap.rssi;
        record.aps[index].auth = ap.auth;
        record.count += 1;
    }
    record
}

fn clear_scan(domain: &DomainHandle) {
    let key = WIFI_DEVICE_UID.to_le_bytes();
    let meta = FactMeta {
        source: SOURCE_WIFI as u8,
        value: Value::NONE,
    };
    if let Err(error) = domain.entity_remove(ENTITY_WIFI_SCAN, &key, meta) {
        if error != ha_domain::Error::NotFound {
            log::warn!("wifi: scan clear failed: {error:?}");
        }
    }
}
fn publish_scan(domain: &DomainHandle, results: &[ScanAp]) {
    let record = scan_record(results);
    let meta = FactMeta {
        source: SOURCE_WIFI as u8,
        value: Value::NONE,
    };
    let key = WIFI_DEVICE_UID.to_le_bytes();
    if let Err(error) = domain.entity_put(ENTITY_WIFI_SCAN, &key, &record.encode(), meta) {
        log::error!("wifi: scan write failed: {error:?}");
    }
}
#[cfg(test)]
mod tests {
    use super::*;

    fn known(id: u32) -> (u32, WifiKnownRecord) {
        (id, WifiKnownRecord::default())
    }

    #[test]
    fn status_state_follows_v2_states() {
        assert_eq!(next_status_state(0, false, false), 0);
        assert_eq!(next_status_state(0, true, false), 3);
        assert_eq!(next_status_state(1, false, false), 1);
        assert_eq!(next_status_state(1, true, false), 1);
        assert_eq!(next_status_state(2, true, false), 3);
        assert_eq!(next_status_state(2, false, false), 2);
        assert_eq!(next_status_state(2, false, true), 4);
        assert_eq!(next_status_state(3, false, false), 0);
        assert_eq!(next_status_state(4, true, false), 3);
    }

    #[test]
    fn scan_record_preserves_auth() {
        let record = scan_record(&[ScanAp {
            ssid: "lab".into(),
            rssi: -42,
            auth: 3,
        }]);
        assert_eq!(record.count, 1);
        assert_eq!(record.aps[0].auth, 3);
        assert_eq!(&record.aps[0].ssid[..3], b"lab");
        assert_eq!(record.aps[0].rssi, -42);
    }

    #[test]
    fn known_ids_follow_v2_max_plus_one() {
        assert_eq!(next_known_id(&[]), Some(1));
        assert_eq!(next_known_id(&[known(1), known(4)]), Some(5));
        assert_eq!(
            next_known_id(&[
                known(1),
                known(2),
                known(3),
                known(4),
                known(5),
                known(6),
                known(7),
                known(8)
            ]),
            None
        );
    }
}
