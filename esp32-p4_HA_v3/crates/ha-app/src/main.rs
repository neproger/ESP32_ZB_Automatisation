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
use ha_domain::{Domain, DomainHandle, FactMeta, FactTarget, Filter};
use zigbee_devices::DomainAdapter;

mod automation_task;
mod capabilities;
mod configure;
mod display;
mod exec;
mod frames;
mod intake;
mod interview;
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

    let mut domain = Domain::new_with_clock(64, 8, 64, now_ms).expect("domain");
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
    store::init();
    if let Err(error) = DomainAdapter::new(&mut domain) {
        log::error!("domain: table init failed: {error:?}");
    }
    store::restore_domain(&mut domain);
    ensure_system_location(&mut domain);
    ensure_settings(&mut domain);
    if let Some(count) = option_env!("HA_FORCE_COMPACT") {
        for _ in 0..count.parse::<u32>().unwrap_or(0) {
            store::compact_for_test();
        }
    }
    let domain = DomainHandle::new(domain);
    let (zigbee, zigbee_state) = zigbee_service::start(domain.clone());
    {
        let command_state = std::sync::Arc::clone(&zigbee_state);
        if let Err(error) = domain.with_mut(|domain| {
            domain
                .register_command(COMMAND_PROPERTY, move |args| {
                    let command =
                        CanonicalCommand::decode(args).ok_or(ha_domain::Error::InvalidSize)?;
                    command_state.lock().unwrap().submit(command)
                })
                .expect("domain command executor");
            domain
                .register_command(COMMAND_WIFI_SCAN, |_args| wifi::request_scan())
                .expect("wifi scan executor");
            domain
                .register_command(COMMAND_WIFI_CONNECT, wifi::connect_from_args)
                .expect("wifi connect executor");
        }) {
            log::error!("domain: command registration failed: {error:?}");
        }
    }
    if display::start() {
        domain.with(|domain| display::rebuild(domain));
    }
    system::start(domain.with(read_location).unwrap_or_default());
    std::thread::Builder::new()
        .name("zigbee-mainloop".into())
        .stack_size(8192)
        .spawn(|| unsafe { zb_poc_run() })
        .expect("zigbee mainloop thread");
    wifi::start(domain.clone());
    let dispatcher_domain = domain.clone();
    let dispatch = domain.lock().dispatch_handle();
    std::thread::Builder::new()
        .name("dispatcher".into())
        .stack_size(4096)
        .spawn(move || loop {
            dispatch.wait();
            if let Err(error) = dispatcher_domain.with_mut(|domain| domain.dispatch_once()) {
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
            display_domain.with(|domain| {
                display::drain(domain, &display_rx);
                display::tick();
            });
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
            web_domain.with(|domain| web::drain(domain, &web_rx));
            web::flush();
        })
        .expect("web drain thread");
    automation_task::spawn(domain.clone(), automation_rx);
    log::info!("ha-app ready: device table + journal, waiting for button events");

    let mut last_minute: Option<(u16, u8)> = None;
    let mut last_wifi_poll = 0u64;
    let mut last_weather: Option<u8> = None;
    let mut last_subscriber_drops = 0u64;
    loop {
        while let Some(request) = intake::take() {
            let mut domain = domain.lock();
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
                    if let Err(error) = post_command(&mut domain, command, SOURCE_UI as u8) {
                        log::warn!(
                            "command rejected: entity={entity:016x} property={property} error={error:?}"
                        );
                    }
                }
                intake::Request::RenameEntity { entity, name } => {
                    rename_entity(&mut domain, entity, &name);
                }
                intake::Request::RenameDevice { device, name } => {
                    rename_device(&mut domain, device, &name);
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
                        continue;
                    }
                    record[384] = u8::from(enabled);
                    let mut sink = StoreSink;
                    let Ok(mut adapter) = DomainAdapter::with_sink(&mut domain, &mut sink) else {
                        continue;
                    };
                    if let Err(error) =
                        adapter.put_record(ha_domain::canonical::ENTITY_AUTOMATION, &key, &record)
                    {
                        log::warn!("display: automation {id:016x} write failed: {error:?}");
                        continue;
                    }
                    drop(adapter);
                }
                intake::Request::SetSettings { brightness } => {
                    let key = [SETTINGS_ID];
                    let mut settings = read_settings(&domain);
                    settings.brightness_pct = brightness;
                    let record = settings.encode();
                    let mut sink = StoreSink;
                    let Ok(mut adapter) = DomainAdapter::with_sink(&mut domain, &mut sink) else {
                        continue;
                    };
                    if let Err(error) = adapter.put_record(ENTITY_SETTINGS, &key, &record) {
                        log::warn!("display: settings write failed: {error:?}");
                        continue;
                    }
                    drop(adapter);
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
                    let mut sink = StoreSink;
                    let Ok(mut adapter) = DomainAdapter::with_sink(&mut domain, &mut sink) else {
                        log::error!("domain: adapter init failed");
                        continue;
                    };
                    match adapter.put_record(entity, &key, &record) {
                        Ok(_) => {}
                        Err(error) => {
                            log::warn!("domain: upsert entity={entity} failed: {error:?}")
                        }
                    }
                    drop(adapter);
                    if is_location {
                        if let Some(location) = read_location(&domain) {
                            system::set_location(location);
                        }
                    }
                }
                intake::Request::Remove { entity, key } => {
                    let mut sink = StoreSink;
                    let Ok(mut adapter) = DomainAdapter::with_sink(&mut domain, &mut sink) else {
                        log::error!("domain: adapter init failed");
                        continue;
                    };
                    match entity {
                        ha_domain::canonical::ENTITY_GROUP => {
                            let Some(id) = key
                                .get(..8)
                                .and_then(|bytes| bytes.try_into().ok())
                                .map(u64::from_le_bytes)
                            else {
                                continue;
                            };
                            match adapter.remove_group(id) {
                                Ok(count) => {
                                    log::info!("domain: removed group={id:016x} records={count}")
                                }
                                Err(error) => {
                                    log::warn!("domain: remove group={id:016x} failed: {error:?}")
                                }
                            }
                        }
                        ha_domain::canonical::ENTITY_AUTOMATION => {
                            let Some(id) = key
                                .get(..8)
                                .and_then(|bytes| bytes.try_into().ok())
                                .map(u64::from_le_bytes)
                            else {
                                continue;
                            };
                            match adapter.remove_automation(id) {
                                Ok(count) => log::info!(
                                    "domain: removed automation={id:016x} records={count}"
                                ),
                                Err(error) => log::warn!(
                                    "domain: remove automation={id:016x} failed: {error:?}"
                                ),
                            }
                        }
                        _ => match adapter.remove_record(entity, &key) {
                            Ok(_) => {}
                            Err(error) => {
                                log::warn!("domain: remove entity={entity} failed: {error:?}")
                            }
                        },
                    }
                    drop(adapter);
                }
            }
        }
        while let Some(nav) = display::take_nav() {
            let domain = domain.lock();
            match nav {
                display::Nav::WifiScan => {
                    if let Err(error) = wifi::request_scan() {
                        log::warn!("wifi: scan request rejected ({error:?})");
                    } else {
                        log::info!("wifi: scan requested");
                        display::wifi_scan_requested(&domain);
                    }
                }
                display::Nav::OpenWifi => {
                    if let Some(results) = wifi::take_scan() {
                        display::set_wifi_scan(results);
                    }
                    display::navigate(&domain, display::Nav::OpenWifi);
                }
                _ => display::navigate(&domain, nav),
            }
        }
        if let Some(results) = wifi::take_scan() {
            log::info!("wifi: scan result {} networks", results.len());
            let domain = domain.lock();
            display::wifi_scan_done(&domain, results);
        }
        while let Some(update) = system::take() {
            let mut domain = domain.lock();
            match update {
                system::Update::Location(location) => apply_system_location(&mut domain, location),
                system::Update::Weather(weather) => {
                    let changed = last_weather != Some(weather.condition);
                    last_weather = Some(weather.condition);
                    apply_weather(&mut domain, weather);
                    if changed {
                        publish_event(
                            &mut domain,
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
            let mut domain = domain.lock();
            apply_capabilities(&mut domain, entity, capabilities);
        }
        if let Some((minutes, weekday)) = domain.with(local_minute) {
            if last_minute != Some((minutes, weekday)) {
                if last_minute.is_none() {
                    log::info!("time: local clock ready (time rules use Location tz)");
                }
                last_minute = Some((minutes, weekday));
                if let Err(error) = domain.with_mut(|domain| {
                    publish_system_time(domain, minutes, weekday);
                    publish_event(
                        domain,
                        CanonicalEvent {
                            entity: ENTITY_SYSTEM,
                            event: EVENT_MINUTE_TICK,
                        },
                    );
                }) {
                    log::warn!("domain: time publish failed: {error:?}");
                }
            }
        }
        let now = now_ms();
        if now.saturating_sub(last_wifi_poll) >= 2000 {
            last_wifi_poll = now;
            display::set_wifi(wifi::rssi());
        }
        let subscriber_drops = domain.with(|domain| domain.subscriber_drops());
        if subscriber_drops != last_subscriber_drops {
            log::warn!("domain: subscriber drops={subscriber_drops}");
            last_subscriber_drops = subscriber_drops;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn apply_capabilities(
    domain: &mut Domain,
    entity: u64,
    capabilities: ha_domain::canonical::CapabilitiesRecord,
) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        return;
    };
    let changed = match adapter.write_capabilities(entity, &capabilities) {
        Ok(changed) => changed,
        Err(error) => {
            log::error!("domain: capabilities {entity:016x} failed: {error:?}");
            return;
        }
    };
    drop(adapter);
    let _ = changed;
}

fn publish_event(domain: &mut Domain, event: CanonicalEvent) {
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
struct StoreSink;

impl zigbee_devices::RecordSink for StoreSink {
    fn put(&mut self, entity: u32, key: &[u8], record: &[u8]) {
        store::save_domain(entity, key, record);
    }

    fn remove(&mut self, entity: u32, key: &[u8]) {
        store::remove_domain(entity, key);
    }
}

impl ha_device::Sink for StoreSink {
    fn put(&mut self, entity: u32, key: &[u8], record: &[u8]) {
        store::save_domain(entity, key, record);
    }

    fn remove(&mut self, entity: u32, key: &[u8]) {
        store::remove_domain(entity, key);
    }
}

fn post_command(
    domain: &mut Domain,
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

fn ensure_system_location(domain: &mut Domain) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let mut current = [0u8; LOCATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_LOCATION, &key, &mut current)
        .is_ok()
    {
        return;
    }
    let record = LocationRecord::default().encode();
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: location table init failed");
        return;
    };
    match adapter.put_record(ENTITY_LOCATION, &key, &record) {
        Ok(_) => log::info!("domain: location seeded"),
        Err(error) => log::warn!("domain: location seed failed: {error:?}"),
    }
}

/// Local wall-clock minute for automation Time triggers. A manual offset is
/// applied; auto timezone falls back to UTC until GeoIP exists.

fn read_settings(domain: &Domain) -> SettingsRecord {
    let key = [SETTINGS_ID];
    let mut bytes = [0u8; SETTINGS_RECORD_LEN];
    domain
        .entity_get(ENTITY_SETTINGS, &key, &mut bytes)
        .ok()
        .and_then(|()| SettingsRecord::decode(&bytes))
        .unwrap_or_default()
}

fn ensure_settings(domain: &mut Domain) {
    let key = [SETTINGS_ID];
    let mut bytes = [0u8; SETTINGS_RECORD_LEN];
    if domain.entity_get(ENTITY_SETTINGS, &key, &mut bytes).is_ok() {
        return;
    }
    let record = SettingsRecord::default().encode();
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        return;
    };
    match adapter.put_record(ENTITY_SETTINGS, &key, &record) {
        Ok(_) => log::info!("domain: settings seeded"),
        Err(error) => log::warn!("domain: settings seed failed: {error:?}"),
    }
}

fn read_location(domain: &Domain) -> Option<LocationRecord> {
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

fn apply_system_location(domain: &mut Domain, location: LocationRecord) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let record = location.encode();
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        return;
    };
    match adapter.put_record(ENTITY_LOCATION, &key, &record) {
        Ok(_) => {}
        Err(error) => log::warn!("system: location write failed: {error:?}"),
    }
    drop(adapter);
    system::set_location(location);
}

fn apply_weather(domain: &mut Domain, weather: WeatherRecord) {
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
fn publish_system_time(domain: &mut Domain, minutes: u16, weekday: u8) {
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

fn local_minute(domain: &Domain) -> Option<(u16, u8)> {
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

fn rename_entity(domain: &mut Domain, entity: u64, name: &str) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: adapter init failed");
        return;
    };
    match adapter.rename_entity(entity, name) {
        Ok(_) => {
            log::info!("domain: renamed entity={entity:016x} name={name}");
        }
        Err(error) => log::warn!("domain: rename entity={entity:016x} failed: {error:?}"),
    }
}

fn rename_device(domain: &mut Domain, device: u64, name: &str) {
    let mut sink = StoreSink;
    match ha_device::rename(domain, SOURCE_UI as u8, device, name, &mut sink) {
        Ok(_) => {
            log::info!("device: renamed device={device:016x} name={name}");
        }
        Err(error) => log::warn!("device: rename device={device:016x} failed: {error:?}"),
    }
}
