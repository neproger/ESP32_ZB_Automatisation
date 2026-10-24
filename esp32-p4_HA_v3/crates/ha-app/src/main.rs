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

use std::sync::mpsc;
use std::time::Duration;

use ha_domain::{Domain, EntityDesc, EntityType, Error, Fact, FactKind, FactMeta, Filter, Value};
use zigbee_devices::{DeviceAdapter, Property, RawZclEvent, TuyaButton};

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

    let started = unsafe { zb_poc_start() };
    if started != 0 {
        log::error!("zb_poc_start failed: {started}");
        panic!("zigbee stack did not start");
    }
    std::thread::Builder::new()
        .name("zigbee-mainloop".into())
        .stack_size(8192)
        .spawn(|| unsafe { zb_poc_run() })
        .expect("zigbee mainloop thread");
    log::info!("ha-app ready: device table + journal, waiting for button events");

    loop {
        let mut event = PocEvent::default();
        if unsafe { zb_poc_poll(&mut event) } != 0 {
            handle_event(&mut domain, &event, &delta_rx);
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn handle_event(domain: &mut Domain, event: &PocEvent, deltas: &mpsc::Receiver<Fact>) {
    let payload_len = usize::from(event.payload_len.min(8));
    let raw = RawZclEvent {
        eui64: event.extended_addr,
        endpoint: event.endpoint,
        cluster: event.cluster_id,
        command: event.command_id,
        payload: &event.payload[..payload_len],
    };
    let update = match TuyaButton.decode(&raw) {
        Some(update) => update,
        None => {
            log::info!(
                "ignored eui64={:016x} cluster=0x{:04x} cmd=0x{:02x} payload_len={payload_len}",
                event.extended_addr,
                event.cluster_id,
                event.command_id
            );
            return;
        }
    };
    let action = match update.property {
        Property::Button(action) => action,
    };
    let key = update.device;

    let mut record = [0u8; DEVICE_RECORD_LEN];
    match domain.entity_get(DEVICE, &key, &mut record) {
        Ok(()) | Err(Error::NotFound) => {}
        Err(err) => {
            log::error!("device record read failed: {err:?}");
            return;
        }
    }

    let count = u16::from_le_bytes([record[2], record[3]]).wrapping_add(1);
    record[0] = action as u8;
    record[2..4].copy_from_slice(&count.to_le_bytes());

    let changed = match domain.entity_put(
        DEVICE,
        &key,
        &record,
        FactMeta {
            source: SOURCE_ZIGBEE,
            value: Value::enum_(action.code()),
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
