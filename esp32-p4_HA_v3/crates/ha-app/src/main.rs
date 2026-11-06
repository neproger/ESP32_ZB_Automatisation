//! First vertical HA V3 slice on hardware.
//!
//! Composition root: starts Domain, Zigbee service, Wi-Fi, System, Web,
//! Display and Automation. Zigbee frames/commands stay inside
//! `zigbee_service`; business state reaches Domain as canonical facts.
//! No async runtime: every service owns its task and inbox.
//!
//! PROPERTY_STATE is RAM-only. Persistent device/config records are written to
//! MicroDB (`domain` partition) and restored at boot; state is not. The device
//! registry lives in `mstore`.

use std::sync::mpsc::sync_channel;
use std::time::Duration;

use ha_domain::canonical::{
    CanonicalCommand, LocationRecord, SettingsRecord, COMMAND_PROPERTY, COMMAND_WIFI_CONNECT,
    COMMAND_WIFI_SCAN, ENTITY_ENTITY, ENTITY_LOCATION, ENTITY_SETTINGS, LOCATION_RECORD_LEN,
    SETTINGS_ID, SETTINGS_RECORD_LEN, SOURCE_SYSTEM, SOURCE_UI, SOURCE_ZIGBEE, SYSTEM_DEVICE_UID,
};
use ha_domain::{Domain, DomainHandle, DomainTransaction, FactKind, FactMeta, Filter};

mod capabilities;
mod config;
mod configure;
mod display;
mod domain_tables;
mod exec;
mod frames;
mod intake;
mod interview;
#[cfg(feature = "p4-lock-metrics")]
mod p4_probe;
mod removal_queue;
mod store;
mod web;
mod zigbee_service;

extern "C" {
    fn zb_poc_start() -> i32;
    fn zb_poc_run();
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    store::init();

    let storage = match store::DomainStorageProvider::new() {
        Ok(storage) => storage,
        Err(error) => {
            log::error!("domain: storage open failed: {error:?}");
            return;
        }
    };
    let mut domain = match Domain::new_with_storage(64, 8, 64, now_ms, storage) {
        Ok(domain) => domain,
        Err(error) => {
            log::error!("domain: init failed: {error:?}");
            return;
        }
    };
    if let Err(error) = domain_tables::register(&mut domain) {
        log::error!("domain: table init failed: {error:?}");
        return;
    }
    if let Err(error) = domain.restore() {
        log::error!("domain: restore failed: {error:?}");
        return;
    }

    let (display_tx, display_rx) = sync_channel(64);
    let (web_tx, web_rx) = sync_channel(64);
    domain.subscribe_channel(Filter::all(), display_tx);
    let web_filter = Filter {
        kind_mask: FactKind::EntityUpserted.bit()
            | FactKind::EntityRemoved.bit()
            | FactKind::Event.bit(),
        ..Filter::all()
    };
    domain.subscribe_channel(web_filter, web_tx);

    unsafe {
        esp_idf_svc::sys::esp_netif_init();
        esp_idf_svc::sys::esp_event_loop_create_default();
    }

    let started = unsafe { zb_poc_start() };
    if started != 0 {
        log::error!("zb_poc_start failed: {started}");
        panic!("zigbee stack did not start");
    }
    frames::install();
    exec::install();
    interview::install();
    if let Some(count) = option_env!("HA_FORCE_COMPACT") {
        for _ in 0..count.parse::<u32>().unwrap_or(0) {
            store::compact_for_test();
        }
    }
    let domain = DomainHandle::new(domain);
    if !web::start(domain.clone()) {
        log::error!("web server not started");
    }
    ensure_system_location(&domain);
    ensure_settings(&domain);
    let (zigbee, zigbee_state) = zigbee_service::start(domain.clone());
    {
        let command_state = std::sync::Arc::clone(&zigbee_state);
        if let Err(error) = domain.register_command(COMMAND_PROPERTY, move |args| {
            let command = CanonicalCommand::decode(args).ok_or(ha_domain::Error::InvalidSize)?;
            command_state.lock().unwrap().submit(command)
        }) {
            log::error!("domain: command executor registration failed: {error:?}");
        }
        if let Err(error) =
            domain.register_command(COMMAND_WIFI_SCAN, |_args| ha_wifi::request_scan())
        {
            log::error!("domain: wifi scan executor registration failed: {error:?}");
        }
        if let Err(error) =
            domain.register_command(COMMAND_WIFI_CONNECT, ha_wifi::connect_from_args)
        {
            log::error!("domain: wifi connect executor registration failed: {error:?}");
        }
    }
    if !display::start(&domain) {
        log::error!("display not started");
    }
    std::thread::Builder::new()
        .name("zigbee-mainloop".into())
        .stack_size(8192)
        .spawn(|| unsafe { zb_poc_run() })
        .expect("zigbee mainloop thread");
    if let Err(error) = ha_system::start(domain.clone()) {
        log::error!("system: start failed: {error}");
    }
    ha_wifi::start(domain.clone());
    let dispatcher_domain = domain.clone();
    let dispatch = domain.dispatch_handle().expect("domain dispatch handle");
    std::thread::Builder::new()
        .name("dispatcher".into())
        .stack_size(4096)
        .spawn(move || loop {
            dispatch.wait();
            if let Err(error) = dispatcher_domain.dispatch_once() {
                log::error!("domain: dispatcher failed: {error:?}");
            }
        })
        .expect("dispatcher thread");
    // shortcut: display/web drain loops use a 20 ms timeout to keep LVGL ticks
    // alive; the fact inbox itself remains bounded and non-blocking.
    if let Err(error) = ha_automation::start(domain.clone()) {
        log::error!("automation: start failed: {error}");
    }
    #[cfg(feature = "p4-lock-metrics")]
    p4_probe::start(domain.clone());
    log::info!("ha-app ready: device table + journal, waiting for button events");

    let mut last_subscriber_drops = 0u64;
    loop {
        while let Some(request) = intake::take() {
            handle_intake(&domain, &zigbee, request);
        }
        while let Some(nav) = display::take_nav() {
            match nav {
                display::Nav::WifiScan => {
                    if let Err(error) = display::request_wifi_scan() {
                        log::warn!("wifi: scan request rejected ({error:?})");
                    }
                }
                display::Nav::OpenWifi => display::navigate(&domain, display::Nav::OpenWifi),
                _ => display::navigate(&domain, nav),
            }
        }

        while let Some((entity, capabilities)) = capabilities::take_capabilities() {
            if let Err(error) = domain.transaction(|transaction| {
                apply_capabilities(transaction, entity, capabilities).map(|_| ())
            }) {
                log::warn!("domain: capabilities update failed: {error:?}");
            }
        }
        // shortcut: Web/Display drains run on the main loop; dedicated P4 drain threads stalled.
        display::drain(&domain, &display_rx);
        display::tick();
        web::drain(&domain, &web_rx);
        web::flush();
        let subscriber_drops = domain.subscriber_drops().unwrap_or(0);
        if subscriber_drops != last_subscriber_drops {
            log::warn!("domain: subscriber drops={subscriber_drops}");
            last_subscriber_drops = subscriber_drops;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn handle_intake(domain: &DomainHandle, zigbee: &zigbee_service::Handle, request: intake::Request) {
    match request {
        intake::Request::Command {
            entity,
            property,
            action,
            value,
        } => {
            let command = CanonicalCommand {
                entity,
                property,
                action,
                value,
            };
            if let Err(error) = post_command(domain, command, SOURCE_UI as u8) {
                log::warn!(
                    "command rejected: entity={entity:016x} property={property} error={error:?}"
                );
            }
        }
        intake::Request::RenameEntity { entity, name } => {
            if let Err(error) = domain.transaction(|transaction| {
                let key = entity.to_le_bytes();
                let mut current = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
                transaction.entity_get(ENTITY_ENTITY, &key, &mut current)?;
                let device = u64::from_le_bytes(current[..8].try_into().expect("entity owner"));
                let encoded = ha_domain::canonical::EntityRecord::new(device, &name).encode();
                transaction
                    .entity_put(
                        ENTITY_ENTITY,
                        &key,
                        &encoded,
                        FactMeta {
                            source: SOURCE_UI as u8,
                            value: ha_domain::Value::NONE,
                        },
                    )
                    .map(|_| ())
            }) {
                log::warn!("domain: entity rename failed: {error:?}");
            }
        }
        intake::Request::RenameDevice { device, name } => {
            if let Err(error) = domain.transaction(|transaction| {
                ha_device::rename(transaction, SOURCE_UI as u8, device, &name).map(|_| ())
            }) {
                log::warn!("domain: device rename failed: {error:?}");
            }
        }
        intake::Request::RemoveDevice { device } => {
            if let Err(error) = zigbee.remove_device(device) {
                log::warn!("device: remove {device:016x} rejected: {error:?}");
            }
        }
        intake::Request::Upsert {
            entity,
            key,
            record,
        } => {
            let result = if entity == ENTITY_LOCATION {
                let meta = FactMeta {
                    source: SOURCE_UI as u8,
                    value: ha_domain::Value::NONE,
                };
                domain.entity_put(entity, &key, &record, meta).map(|_| ())
            } else {
                config::put_record(domain, entity, &key, &record).map(|_| ())
            };
            if let Err(error) = result {
                log::warn!("domain: upsert entity={entity} failed: {error:?}");
            }
        }
        intake::Request::Remove { entity, key } => {
            let id = || {
                key.get(..8)
                    .and_then(|bytes| bytes.try_into().ok())
                    .map(u64::from_le_bytes)
                    .ok_or(ha_domain::Error::InvalidSize)
            };
            match entity {
                ha_domain::canonical::ENTITY_GROUP => {
                    match id().and_then(|id| config::remove_group(domain, id)) {
                        Ok(count) => log::info!("config: removed group records={count}"),
                        Err(error) => log::warn!("config: group removal failed: {error:?}"),
                    }
                }
                ha_domain::canonical::ENTITY_AUTOMATION => {
                    match id().and_then(|id| config::remove_automation(domain, id)) {
                        Ok(count) => log::info!("config: removed automation records={count}"),
                        Err(error) => log::warn!("config: automation removal failed: {error:?}"),
                    }
                }
                _ => {
                    if let Err(error) = config::remove_record(domain, entity, &key) {
                        log::warn!("domain: remove entity={entity} failed: {error:?}");
                    }
                }
            }
        }
    }
}

fn apply_capabilities(
    domain: &mut DomainTransaction<'_>,
    entity: u64,
    capabilities: ha_domain::canonical::CapabilitiesRecord,
) -> Result<bool, ha_domain::Error> {
    ha_device::set_capabilities(domain, SOURCE_ZIGBEE as u8, entity, &capabilities)
}

fn post_command(
    domain: &DomainHandle,
    command: CanonicalCommand,
    source: u8,
) -> Result<(), ha_domain::Error> {
    let args = command.encode();
    let meta = FactMeta {
        source,
        value: ha_domain::Value::NONE,
    };
    domain.post(COMMAND_PROPERTY, &args, None, meta)
}
fn ensure_system_location(domain: &DomainHandle) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let mut current = [0u8; LOCATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_LOCATION, &key, &mut current)
        .is_ok()
    {
        return;
    }
    let record = LocationRecord::default().encode();
    let meta = FactMeta {
        source: SOURCE_SYSTEM as u8,
        value: ha_domain::Value::NONE,
    };
    match domain.entity_put(ENTITY_LOCATION, &key, &record, meta) {
        Ok(_) => log::info!("domain: location seeded"),
        Err(error) => log::warn!("domain: location seed failed: {error:?}"),
    }
}

fn ensure_settings(domain: &DomainHandle) {
    let key = [SETTINGS_ID];
    let mut bytes = [0u8; SETTINGS_RECORD_LEN];
    if domain.entity_get(ENTITY_SETTINGS, &key, &mut bytes).is_ok() {
        return;
    }
    let record = SettingsRecord::default().encode();
    match config::put_record(domain, ENTITY_SETTINGS, &key, &record) {
        Ok(_) => log::info!("domain: settings seeded"),
        Err(error) => log::warn!("domain: settings seed failed: {error:?}"),
    }
}

fn now_ms() -> u64 {
    (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000).max(0) as u64
}
