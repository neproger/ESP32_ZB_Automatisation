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
    CanonicalCommand, EventId, LocationRecord, SettingsRecord, StateKey, StateRecord,
    WeatherRecord, COMMAND_PROPERTY, COMMAND_WIFI_CONNECT, COMMAND_WIFI_SCAN, ENTITY_ENTITY,
    ENTITY_LOCATION, ENTITY_PROPERTY_STATE, ENTITY_SETTINGS, ENTITY_SYSTEM, ENTITY_WEATHER,
    EVENT_MINUTE_TICK, EVENT_WEATHER_CHANGED, LOCATION_RECORD_LEN, PROPERTY_SYSTEM_HOUR,
    PROPERTY_SYSTEM_MINUTE, PROPERTY_SYSTEM_MINUTES_OF_DAY, PROPERTY_SYSTEM_TIME_STATUS,
    PROPERTY_SYSTEM_TIME_UTC, PROPERTY_SYSTEM_TZ_OFFSET, PROPERTY_SYSTEM_WEEKDAY,
    PROPERTY_SYSTEM_WEEKDAY_MASK, SETTINGS_ID, SETTINGS_RECORD_LEN, SOURCE_SYSTEM, SOURCE_UI,
    SOURCE_ZIGBEE, SYSTEM_DEVICE_UID, WEATHER_DEVICE_UID,
};
use ha_domain::{Domain, DomainHandle, DomainTransaction, FactMeta, FactTarget, Filter};

mod automation_task;
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
mod store;
mod system;
mod web;
mod wifi;
mod zigbee_service;

#[derive(Clone, Copy)]
struct CanonicalEvent {
    entity: u64,
    event: EventId,
}

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
    let (automation_tx, automation_rx) = sync_channel(64);
    domain.subscribe_channel(Filter::all(), display_tx);
    domain.subscribe_channel(Filter::all(), web_tx);
    domain.subscribe_channel(Filter::all(), automation_tx);

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
    if let Some(count) = option_env!("HA_FORCE_COMPACT") {
        for _ in 0..count.parse::<u32>().unwrap_or(0) {
            store::compact_for_test();
        }
    }
    let domain = DomainHandle::new(domain);
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
        if let Err(error) = domain.register_command(COMMAND_WIFI_SCAN, |_args| wifi::request_scan())
        {
            log::error!("domain: wifi scan executor registration failed: {error:?}");
        }
        if let Err(error) = domain.register_command(COMMAND_WIFI_CONNECT, wifi::connect_from_args) {
            log::error!("domain: wifi connect executor registration failed: {error:?}");
        }
    }
    if display::start() {
        display::rebuild(&domain);
    }
    system::start(read_location(&domain).unwrap_or_default());
    std::thread::Builder::new()
        .name("zigbee-mainloop".into())
        .stack_size(8192)
        .spawn(|| unsafe { zb_poc_run() })
        .expect("zigbee mainloop thread");
    wifi::start(domain.clone());
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
    let display_domain = domain.clone();
    std::thread::Builder::new()
        .name("display-drain".into())
        .stack_size(8192)
        .spawn(move || loop {
            match display_rx.recv_timeout(Duration::from_millis(20)) {
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
                _ => {}
            }
            display::drain(&display_domain, &display_rx);
            display::tick();
        })
        .expect("display drain thread");
    let web_domain = domain.clone();
    std::thread::Builder::new()
        .name("web-drain".into())
        .stack_size(8192)
        .spawn(move || loop {
            match web_rx.recv_timeout(Duration::from_millis(20)) {
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
                _ => {}
            }
            web::drain(&web_domain, &web_rx);
            web::flush();
        })
        .expect("web drain thread");
    automation_task::spawn(domain.clone(), automation_rx);
    #[cfg(feature = "p4-lock-metrics")]
    p4_probe::start(domain.clone());
    log::info!("ha-app ready: device table + journal, waiting for button events");

    let mut last_minute: Option<(u16, u8)> = None;
    let mut last_weather: Option<u8> = None;
    let mut last_subscriber_drops = 0u64;
    loop {
        while let Some(request) = intake::take() {
            handle_intake(&domain, &zigbee, request);
        }
        while let Some(nav) = display::take_nav() {
            match nav {
                display::Nav::WifiScan => {
                    let meta = FactMeta {
                        source: SOURCE_UI as u8,
                        value: ha_domain::Value::NONE,
                    };
                    if let Err(error) = domain.post(COMMAND_WIFI_SCAN, &[], None, meta) {
                        log::warn!("wifi: scan request rejected ({error:?})");
                    }
                }
                display::Nav::OpenWifi => display::navigate(&domain, display::Nav::OpenWifi),
                _ => display::navigate(&domain, nav),
            }
        }
        while let Some(update) = system::take() {
            match update {
                system::Update::Location(location) => {
                    apply_system_location(&domain, location);
                }
                system::Update::Weather(weather) => {
                    let changed = last_weather != Some(weather.condition);
                    last_weather = Some(weather.condition);
                    apply_weather(&domain, weather);
                    if changed {
                        publish_event(
                            &domain,
                            CanonicalEvent {
                                entity: ENTITY_SYSTEM,
                                event: EVENT_WEATHER_CHANGED,
                            },
                        );
                    }
                }
            }
        }
        while let Some((entity, capabilities)) = capabilities::take_capabilities() {
            if let Err(error) = domain.transaction(|transaction| {
                apply_capabilities(transaction, entity, capabilities).map(|_| ())
            }) {
                log::warn!("domain: capabilities update failed: {error:?}");
            }
        }
        if let Some((minutes, weekday)) = local_minute(&domain) {
            if last_minute != Some((minutes, weekday)) {
                if last_minute.is_none() {
                    log::info!("time: local clock ready (time rules use Location tz)");
                }
                last_minute = Some((minutes, weekday));
                publish_system_time(&domain, minutes, weekday);
                publish_event(
                    &domain,
                    CanonicalEvent {
                        entity: ENTITY_SYSTEM,
                        event: EVENT_MINUTE_TICK,
                    },
                );
            }
        }
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
                let device =
                    u64::from_le_bytes(current[..8].try_into().expect("entity owner"));
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
        intake::Request::SetAutomationEnabled {
            automation: id,
            enabled,
        } => {
            let key = id.to_le_bytes();
            let mut record = [0u8; ha_domain::canonical::AUTOMATION_RECORD_LEN];
            if domain
                .entity_get(ha_domain::canonical::ENTITY_AUTOMATION, &key, &mut record)
                .is_err()
            {
                log::warn!("display: automation {id:016x} not found");
                return;
            }
            record[384] = u8::from(enabled);
            match config::put_record(
                domain,
                ha_domain::canonical::ENTITY_AUTOMATION,
                &key,
                &record,
            ) {
                Ok(_) => {}
                Err(error) => log::warn!("display: automation {id:016x} write failed: {error:?}"),
            }
        }
        intake::Request::SetSettings { brightness } => {
            let key = [SETTINGS_ID];
            let mut settings = read_settings(domain);
            settings.brightness_pct = brightness;
            let record = settings.encode();
            match config::put_record(domain, ENTITY_SETTINGS, &key, &record) {
                Ok(_) => {}
                Err(error) => log::warn!("display: settings write failed: {error:?}"),
            }
        }
        intake::Request::WifiConnect { ssid, password } => {
            let args = wifi::encode_connect(&ssid, &password);
            let meta = FactMeta {
                source: SOURCE_UI as u8,
                value: ha_domain::Value::NONE,
            };
            if let Err(error) = domain.post(COMMAND_WIFI_CONNECT, &args, None, meta) {
                log::warn!("wifi: connect rejected: {error:?}");
            }
        }
        intake::Request::Upsert {
            entity,
            key,
            record,
        } => {
            let is_location = entity == ha_domain::canonical::ENTITY_LOCATION;
            if let Err(error) = config::put_record(domain, entity, &key, &record) {
                log::warn!("domain: upsert entity={entity} failed: {error:?}");
            }
            if is_location {
                if let Some(location) = read_location(domain) {
                    system::set_location(location);
                }
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

fn publish_event(domain: &DomainHandle, event: CanonicalEvent) {
    let key = event.entity.to_le_bytes();
    let payload = event.event.to_le_bytes();
    match domain.payload_put(
        Some(FactTarget {
            entity: ENTITY_ENTITY,
            key: &key,
        }),
        FactMeta {
            source: SOURCE_ZIGBEE as u8,
            value: ha_domain::Value::u32(event.event),
        },
        &payload,
    ) {
        Ok(reference) => {
            log::info!(
                "domain: event entity={:016x} id={} payload={reference}",
                event.entity,
                event.event
            );
        }
        Err(error) => log::error!("domain: event write failed: {error:?}"),
    }
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
    match config::put_record(domain, ENTITY_LOCATION, &key, &record) {
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

fn read_settings(domain: &DomainHandle) -> SettingsRecord {
    let key = [SETTINGS_ID];
    let mut bytes = [0u8; SETTINGS_RECORD_LEN];
    domain
        .entity_get(ENTITY_SETTINGS, &key, &mut bytes)
        .ok()
        .and_then(|()| SettingsRecord::decode(&bytes))
        .unwrap_or_default()
}

fn read_location(domain: &DomainHandle) -> Option<LocationRecord> {
    let mut bytes = [0u8; LOCATION_RECORD_LEN];
    domain
        .entity_get(
            ENTITY_LOCATION,
            &SYSTEM_DEVICE_UID.to_le_bytes(),
            &mut bytes,
        )
        .ok()?;
    LocationRecord::decode(&bytes)
}

fn apply_system_location(domain: &DomainHandle, location: LocationRecord) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let record = location.encode();
    if let Err(error) = config::put_record(domain, ENTITY_LOCATION, &key, &record) {
        log::warn!("system: location write failed: {error:?}");
    }
    system::set_location(location);
}

fn apply_weather(domain: &DomainHandle, weather: WeatherRecord) {
    let key = WEATHER_DEVICE_UID.to_le_bytes();
    let record = weather.encode();
    let meta = FactMeta {
        source: SOURCE_SYSTEM as u8,
        value: ha_domain::Value::NONE,
    };
    if let Err(error) = domain.entity_put(ENTITY_WEATHER, &key, &record, meta) {
        log::warn!("system: weather write failed: {error:?}");
    }
}

/// Publish the V2 system time properties (RAM only, no flash writes every minute).
fn publish_system_time(domain: &DomainHandle, minutes: u16, weekday: u8) {
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_secs() as u32);
    let offset = read_location(domain)
        .map(|location| i32::from(location.effective_tz_offset_min()))
        .unwrap_or(0);
    let entries = [
        (PROPERTY_SYSTEM_TIME_UTC, ha_domain::Value::u32(seconds)),
        (PROPERTY_SYSTEM_TIME_STATUS, ha_domain::Value::u32(2)),
        (
            PROPERTY_SYSTEM_HOUR,
            ha_domain::Value::u32(u32::from(minutes / 60)),
        ),
        (
            PROPERTY_SYSTEM_MINUTE,
            ha_domain::Value::u32(u32::from(minutes % 60)),
        ),
        (
            PROPERTY_SYSTEM_WEEKDAY,
            ha_domain::Value::u32(u32::from(weekday)),
        ),
        (
            PROPERTY_SYSTEM_WEEKDAY_MASK,
            ha_domain::Value::u32(1u32 << weekday),
        ),
        (
            PROPERTY_SYSTEM_MINUTES_OF_DAY,
            ha_domain::Value::u32(u32::from(minutes)),
        ),
        (PROPERTY_SYSTEM_TZ_OFFSET, ha_domain::Value::i32(offset)),
    ];
    let meta = FactMeta {
        source: SOURCE_SYSTEM as u8,
        value: ha_domain::Value::NONE,
    };
    for (property, value) in entries {
        let key = StateKey {
            entity: ENTITY_SYSTEM,
            property,
        }
        .to_bytes();
        let record = StateRecord { value }.encode();
        let _ = domain.entity_put(ENTITY_PROPERTY_STATE, &key, &record, meta);
    }
}

fn local_minute(domain: &DomainHandle) -> Option<(u16, u8)> {
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .ok()?
        .as_secs();
    // Before SNTP the clock is at 1970; do not fire time rules at 00:00.
    if seconds < 1_600_000_000 {
        return None;
    }
    let mut bytes = [0u8; LOCATION_RECORD_LEN];
    let offset = domain
        .entity_get(
            ENTITY_LOCATION,
            &SYSTEM_DEVICE_UID.to_le_bytes(),
            &mut bytes,
        )
        .ok()
        .and_then(|()| LocationRecord::decode(&bytes))
        .map(|location| location.effective_tz_offset_min())
        .unwrap_or(0);
    let local = i64::try_from(seconds).ok()? + i64::from(offset) * 60;
    if local < 0 {
        return None;
    }
    let local = local as u64;
    let weekday = ((local / 86_400 + 3) % 7) as u8;
    let minutes = ((local % 86_400) / 60) as u16;
    Some((minutes, weekday))
}

fn now_ms() -> u64 {
    (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000).max(0) as u64
}
