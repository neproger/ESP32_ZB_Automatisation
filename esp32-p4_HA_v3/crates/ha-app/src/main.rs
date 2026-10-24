//! First vertical HA V3 slice on hardware.
//!
//! Zigbee PoC (C shim + bounded queue) -> borrowed `PocEvent` -> `zigbee-devices`
//! adapter (normalized action) -> Domain entity record -> MicroDB slot -> journal
//! fact -> subscriber. No intermediate DTO chain and no async runtime: one poll
//! loop owns the Domain.
//!
//! Device record (12 bytes, the same shape as the host test):
//! `[0]` last button action, `[1]` reserved, `[2..4]` press count (LE),
//! `[8..12]` reserved. Every press changes the count, so `version` must grow.

use std::ffi::{c_char, CStr, CString};
use std::sync::mpsc;
use std::time::Duration;

use ha_domain::{Domain, EntityDesc, EntityType, Error, Fact, FactKind, FactMeta, Filter, Value};

mod exec;
mod frames;
mod interview;
mod store;
mod web;
mod configure;

const DEVICE: EntityType = 1;
const SOURCE_ZIGBEE: u8 = 1;
const DEVICE_RECORD_LEN: usize = 12;

/// Mirror of `poc_event_t` from the C shim; copied once across the FFI boundary.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PocEvent {
    extended_addr: u64,
    short_addr: u16,
    cluster_id: u16,
    endpoint: u8,
    command_id: u8,
    payload_len: u8,
    payload: [u8; 8],
}

extern "C" {
    fn ha_wifi_start(ssid: *const c_char, password: *const c_char) -> i32;
    fn ha_wifi_ip(out: *mut c_char, len: usize) -> i32;
    fn zb_poc_start() -> i32;
    fn zb_poc_run();
    fn zb_poc_poll(event: *mut PocEvent) -> i32;
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    let mut domain = Domain::new(32, 8, 64).expect("domain");
    domain
        .register_entity(EntityDesc {
            entity: DEVICE,
            key_size: 8,
            record_size: DEVICE_RECORD_LEN,
            capacity: 16,
            backing: ha_domain::Backing::Ram,
            persist_key: None,
        })
        .expect("device table");

    let (delta_tx, delta_rx) = mpsc::channel::<Fact>();
    domain.subscribe(
        Filter {
            kind_mask: FactKind::EntityUpserted.bit(),
            ..Filter::entity(DEVICE)
        },
        move |fact| delta_tx.send(*fact).is_ok(),
    );

    unsafe {
        esp_idf_svc::sys::esp_netif_init();
        esp_idf_svc::sys::esp_event_loop_create_default();
    }
    if !web::start() {
        log::error!("web server not started");
    }

    let started = unsafe { zb_poc_start() };
    if started != 0 {
        log::error!("zb_poc_start failed: {started}");
        panic!("zigbee stack did not start");
    }
    frames::install();
    exec::install();
    interview::install();
    store::init();
    let mut executor = Box::new(zigbee_exec::Executor::new());
    let mut transport = exec::EspTransport;
    let (restored, restored_count) = store::device_addresses();
    for (eui, short) in restored.into_iter().take(restored_count) {
        if let Some(profile) = store::lookup(eui) {
            log::info!("registry: restored profile={} (no interview)", profile.id);
            interview::restore(eui, profile);
            configure::ensure(&mut executor, eui, short, now_ms());
        }
    }
    std::thread::Builder::new()
        .name("zigbee-mainloop".into())
        .stack_size(8192)
        .spawn(|| unsafe { zb_poc_run() })
        .expect("zigbee mainloop thread");
    std::thread::Builder::new()
        .name("wifi".into())
        .stack_size(8192)
        .spawn(start_wifi)
        .expect("wifi thread");
    log::info!("ha-app ready: device table + journal, waiting for button events");

    loop {
        let mut event = PocEvent::default();
        if unsafe { zb_poc_poll(&mut event) } != 0 {
            executor.device_awake(event.short_addr);
            ensure_interview(&mut executor, event.extended_addr, event.short_addr);
            handle_event(&mut domain, &event, &delta_rx);
        }
        frames::process_pending();
        if let Some(device) = frames::take_last_device() {
            executor.device_awake(device.1);
            ensure_interview(&mut executor, device.0, device.1);
        }
        while let Some((eui, short)) = interview::next_device() {
            executor.device_awake(short);
            ensure_interview(&mut executor, eui, short);
        }
        executor.tick(&mut transport, now_ms());
        while let Some(result) = executor.take_result() {
            log::info!("exec op={} status={:?}", result.id, result.status);
            interview::on_operation_result(&mut executor, result, now_ms());
            configure::on_result(&mut executor, result, now_ms());
        }
        interview::tick(&mut executor, now_ms());
        configure::tick(&mut executor, now_ms());
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn handle_event(domain: &mut Domain, event: &PocEvent, deltas: &mpsc::Receiver<Fact>) {
    let payload_len = usize::from(event.payload_len.min(8));
    let Some(profile) = interview::selected_profile(event.extended_addr) else {
        log::info!(
            "ignored eui64={:016x} cluster=0x{:04x} cmd=0x{:02x} (no profile yet)",
            event.extended_addr,
            event.cluster_id,
            event.command_id
        );
        return;
    };
    let raw = z2m_rs::RawZcl {
        device: event.extended_addr.to_be_bytes(),
        endpoint: event.endpoint,
        cluster: event.cluster_id,
        command: event.command_id,
        attribute: None,
        payload: &event.payload[..payload_len],
    };
    let Some(decoded) = z2m_rs::decode(profile, &raw) else {
        log::info!(
            "ignored eui64={:016x} cluster=0x{:04x} cmd=0x{:02x} payload_len={payload_len}",
            event.extended_addr,
            event.cluster_id,
            event.command_id
        );
        return;
    };
    let Some(action) = decoded.updates().find_map(|update| match update {
        z2m_rs::Update::Action(action) => Some(*action),
        _ => None,
    }) else {
        log::info!("no action in decoded event");
        return;
    };
    let code = match action {
        z2m_rs::Action::Single => 0u32,
        z2m_rs::Action::Double => 1,
        z2m_rs::Action::Hold => 2,
    };
    let key = event.extended_addr.to_be_bytes();

    let mut record = [0u8; DEVICE_RECORD_LEN];
    match domain.entity_get(DEVICE, &key, &mut record) {
        Ok(()) | Err(Error::NotFound) => {}
        Err(err) => {
            log::error!("device record read failed: {err:?}");
            return;
        }
    }

    let count = u16::from_le_bytes([record[2], record[3]]).wrapping_add(1);
    record[0] = code as u8;
    record[2..4].copy_from_slice(&count.to_le_bytes());

    let changed = match domain.entity_put(
        DEVICE,
        &key,
        &record,
        FactMeta {
            source: SOURCE_ZIGBEE,
            value: Value::enum_(code),
        },
    ) {
        Ok(changed) => changed,
        Err(err) => {
            log::error!("device record write failed: {err:?}");
            return;
        }
    };
    let version = domain
        .entity_meta(DEVICE, &key)
        .map_or(0, |meta| meta.version);

    web::publish(event.extended_addr, code as u8, count);
    let delivered = domain.dispatch_once();
    let mut journal = Vec::new();
    while let Ok(fact) = deltas.try_recv() {
        journal.push(fact.event_id);
    }
    log::info!(
        "device={:016x} press={action:?} count={count} changed={changed} version={version} deltas={delivered} journal={journal:?}",
        event.extended_addr
    );
}

fn start_wifi() {
    let (Some(ssid), Some(password)) = (option_env!("HA_WIFI_SSID"), option_env!("HA_WIFI_PASSWORD"))
    else {
        log::warn!("wifi disabled: set HA_WIFI_SSID/HA_WIFI_PASSWORD at build time");
        return;
    };
    let ssid = CString::new(ssid).expect("ssid");
    let password = CString::new(password).expect("password");

    let err = unsafe { ha_wifi_start(ssid.as_ptr(), password.as_ptr()) };
    if err != 0 {
        log::error!("wifi start failed: {err}");
        return;
    }
    for _ in 0..120 {
        let mut buf = [0 as c_char; 32];
        if unsafe { ha_wifi_ip(buf.as_mut_ptr(), buf.len()) } != 0 {
            let ip = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy();
            log::info!("wifi ip={ip}");
            return;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    log::warn!("wifi: no IPv4 after 60 s");
}
fn now_ms() -> u64 {
    (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000).max(0) as u64
}
fn ensure_interview(executor: &mut zigbee_exec::Executor, eui: u64, short: u16) {
    if eui == 0 {
        return;
    }
    if interview::selected_profile(eui).is_some() {
        configure::ensure(executor, eui, short, now_ms());
        return;
    }
    if let Some(profile) = store::lookup(eui) {
        log::info!("registry: restored profile={} (no interview)", profile.id);
        interview::restore(eui, profile);
        configure::ensure(executor, eui, short, now_ms());
    } else {
        interview::start(executor, (eui, short), now_ms());
    }
}