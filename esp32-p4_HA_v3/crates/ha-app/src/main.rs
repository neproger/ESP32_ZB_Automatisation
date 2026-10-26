//! First vertical HA V3 slice on hardware.
//!
//! In: Zigbee C shim -> bounded inbox -> `z2m-rs` decode -> canonical bridge ->
//! Domain entities/state (RAM) + web mirror -> WebSocket. Out: WS command frame
//! -> bounded intake -> `CommandCore` -> `zigbee-exec` -> C transport.
//! No async runtime: one poll loop owns the Domain, the executor and the queues.
//!
//! The Domain is a RAM cache; its canonical records are written through to
//! MicroDB (`domain` partition) and restored at boot. The registry lives in
//! `mstore`.

use std::ffi::{c_char, CStr, CString};
use std::io::{Read, Write};
use std::time::Duration;

use ha_domain::canonical::{
    LocationRecord, SettingsRecord, StateKey, StateRecord, WeatherRecord, ENTITY_ENTITY,
    ENTITY_LOCATION, ENTITY_PROPERTY_STATE, ENTITY_SETTINGS, ENTITY_SYSTEM, ENTITY_WEATHER,
    EVENT_WEATHER_CHANGED, LOCATION_RECORD_LEN, PROPERTY_SYSTEM_HOUR, PROPERTY_SYSTEM_MINUTE,
    PROPERTY_SYSTEM_MINUTES_OF_DAY, PROPERTY_SYSTEM_TIME_STATUS, PROPERTY_SYSTEM_TIME_UTC,
    PROPERTY_SYSTEM_TZ_OFFSET, PROPERTY_SYSTEM_WEEKDAY, PROPERTY_SYSTEM_WEEKDAY_MASK,
    SETTINGS_ID, SETTINGS_RECORD_LEN, SOURCE_SYSTEM, SOURCE_ZIGBEE, SYSTEM_DEVICE_UID,
    WEATHER_DEVICE_UID,
};
use ha_domain::command::CommandCore;
use ha_domain::{Domain, FactMeta, FactTarget};
use zigbee_devices::{status_from_exec, DomainAdapter};

mod intake;
mod exec;
mod frames;
mod interview;
mod store;
mod web;
mod configure;
mod bridge;
mod display;
mod system;


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
    fn ha_wifi_rssi(rssi: *mut i32) -> i32;
    fn ha_wifi_scan_start() -> i32;
    fn ha_wifi_scan_ready() -> i32;
    fn ha_wifi_scan_clear();
    fn ha_wifi_scan_count() -> i32;
    fn ha_wifi_scan_get(index: i32, ssid: *mut c_char, len: usize, rssi: *mut i32) -> i32;
    fn ha_wifi_connect_ssid(ssid: *const c_char, password: *const c_char) -> i32;
    fn ha_time_start();
    fn zb_poc_start() -> i32;
    fn zb_poc_run();
    fn zb_poc_poll(event: *mut PocEvent) -> i32;
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    let mut domain = Domain::new_with_clock(32, 8, 64, now_ms).expect("domain");

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
    let mut automation = ha_automation::RuleEngine::boxed();
    rebuild_rules(&domain, &mut automation);
    let mut executor = Box::new(zigbee_exec::Executor::new());
    let mut command_core = CommandCore::new();
    let mut bindings: Vec<zigbee_devices::Binding> = Vec::new();
    let mut transport = exec::EspTransport;
    let (restored, restored_count) = store::device_addresses();
    for (eui, short) in restored.into_iter().take(restored_count) {
        if let Some(profile) = store::lookup(eui) {
            log::info!("registry: restored profile={} (no interview)", profile.id);
            interview::restore(eui, profile);
            if store::descriptor(eui).is_some() {
                register_device(&mut domain, &mut bindings, eui, short, profile);
                configure::ensure(&mut executor, eui, short, now_ms());
            } else {
                interview::start(&mut executor, (eui, short), now_ms());
            }
        }
    }
    if let Some(count) = option_env!("HA_FORCE_COMPACT") {
        for _ in 0..count.parse::<u32>().unwrap_or(0) {
            store::compact_for_test();
        }
    }
    if display::start() {
        if let Some(location) = read_location(&domain) {
            display::set_location(&location_name(&location));
        }
        display::set_brightness(read_settings(&domain).brightness_pct);
        display::rebuild(&domain);
    }
    system::start(read_location(&domain).unwrap_or_default());
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

    let mut last_minute: Option<(u16, u8)> = None;
    let mut last_wifi_poll = 0u64;
    let mut last_weather: Option<u8> = None;
    loop {
        let mut event = PocEvent::default();
        if unsafe { zb_poc_poll(&mut event) } != 0 {
            executor.device_awake(event.short_addr);
            zigbee_devices::update_binding_short(
                &mut bindings,
                event.extended_addr,
                event.short_addr,
            );
            ensure_interview(&mut executor, event.extended_addr, event.short_addr);
            ingest_command(&event);
        }
        frames::process_pending();
        if let Some(device) = frames::take_last_device() {
            executor.device_awake(device.1);
            zigbee_devices::update_binding_short(&mut bindings, device.0, device.1);
            ensure_interview(&mut executor, device.0, device.1);
        }
        while let Some((eui, short)) = interview::next_device() {
            executor.device_awake(short);
            zigbee_devices::update_binding_short(&mut bindings, eui, short);
            ensure_interview(&mut executor, eui, short);
        }
        let mut identified = false;
        while let Some((eui, short, profile)) = interview::take_identified() {
            register_device(&mut domain, &mut bindings, eui, short, profile);
            identified = true;
        }
        if identified {
            display::rebuild(&domain);
        }
        let mut display_dirty = false;
        while let Some(request) = intake::take() {
            match request {
                intake::Request::Command {
                    entity,
                    property,
                    action,
                    value,
                } => {
                    for binding in &mut bindings {
                        binding.confirmed = interview::confirmed(binding.eui);
                    }
                    let submitted = zigbee_devices::CommandRouter::new(&mut executor, &bindings)
                        .submit(
                            &mut command_core,
                            entity,
                            property,
                            action,
                            value,
                            now_ms(),
                        );
                    match submitted {
                        Ok(operation) => log::info!(
                            "command: entity={entity:016x} property={property} op={operation}"
                        ),
                        Err(error) => log::warn!(
                            "command rejected: entity={entity:016x} property={property} error={error:?}"
                        ),
                    }
                }
                intake::Request::RenameEntity { entity, name } => {
                    rename_entity(&mut domain, entity, &name);
                    display_dirty = true;
                }
                intake::Request::RenameDevice { device, name } => {
                    rename_device(&mut domain, device, &name);
                    display_dirty = true;
                }
                intake::Request::RemoveDevice { device } => {
                    remove_device(&mut domain, &mut bindings, &mut executor, device);
                    display_dirty = true;
                }
                intake::Request::SetAutomationEnabled {
                    automation: id,
                    enabled,
                } => {
                    let key = id.to_le_bytes();
                    let mut record = [0u8; ha_domain::canonical::AUTOMATION_RECORD_LEN];
                    if domain
                        .entity_get(
                            ha_domain::canonical::ENTITY_AUTOMATION,
                            &key,
                            &mut record,
                        )
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
                    if let Err(error) = adapter.put_record(
                        ha_domain::canonical::ENTITY_AUTOMATION,
                        &key,
                        &record,
                    ) {
                        log::warn!("display: automation {id:016x} write failed: {error:?}");
                        continue;
                    }
                    drop(adapter);
                    web::upsert(ha_web::entity::AUTOMATION, &key, &record);
                    rebuild_rules(&domain, &mut automation);
                    display_dirty = true;
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
                    if let Err(error) =
                        adapter.put_record(ENTITY_SETTINGS, &key, &record)
                    {
                        log::warn!("display: settings write failed: {error:?}");
                        continue;
                    }
                    drop(adapter);
                    web::upsert(ha_web::entity::SETTINGS, &key, &record);
                    display::set_brightness(brightness);
                }
                intake::Request::WifiConnect { ssid, password } => {
                    let Ok(ssid_c) = CString::new(ssid) else {
                        continue;
                    };
                    let password_c = CString::new(password).unwrap_or_default();
                    let rc = unsafe {
                        ha_wifi_connect_ssid(ssid_c.as_ptr(), password_c.as_ptr())
                    };
                    if rc != 0 {
                        log::warn!("wifi: connect request rejected ({rc})");
                    } else {
                        log::info!("wifi: connect requested");
                    }
                }
                intake::Request::Upsert {
                    entity,
                    key,
                    record,
                } => {
                    let is_rule = entity == ha_domain::canonical::ENTITY_AUTOMATION;
                    let is_location = entity == ha_domain::canonical::ENTITY_LOCATION;
                    display_dirty |= display_relevant(entity);
                    let mut sink = StoreSink;
                    let Ok(mut adapter) = DomainAdapter::with_sink(&mut domain, &mut sink) else {
                        log::error!("domain: adapter init failed");
                        continue;
                    };
                    match adapter.put_record(entity, &key, &record) {
                        Ok(changed) => {
                            if changed {
                                if let Ok(frame_type) = u8::try_from(entity) {
                                    web::upsert(frame_type, &key, &record);
                                }
                            }
                        }
                        Err(error) => {
                            log::warn!("domain: upsert entity={entity} failed: {error:?}")
                        }
                    }
                    drop(adapter);
                    if is_rule {
                        rebuild_rules(&domain, &mut automation);
                    }
                    if is_location {
                        if let Some(location) = read_location(&domain) {
                            display::set_location(&location_name(&location));
                            system::set_location(location);
                        }
                    }
                }
                intake::Request::Remove { entity, key } => {
                    let is_rule = entity == ha_domain::canonical::ENTITY_AUTOMATION;
                    display_dirty |= display_relevant(entity);
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
                            web::remove_group(id);
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
                            web::remove_automation(id);
                        }
                        _ => match adapter.remove_record(entity, &key) {
                            Ok(changed) => {
                                if changed {
                                    if let Ok(frame_type) = u8::try_from(entity) {
                                        web::remove(frame_type, &key);
                                    }
                                }
                            }
                            Err(error) => {
                                log::warn!("domain: remove entity={entity} failed: {error:?}")
                            }
                        },
                    }
                    drop(adapter);
                    if is_rule {
                        rebuild_rules(&domain, &mut automation);
                    }
                }
            }
        }
        if display_dirty {
            display::rebuild(&domain);
        }
        while let Some(nav) = display::take_nav() {
            match nav {
                display::Nav::WifiScan => {
                    let rc = unsafe { ha_wifi_scan_start() };
                    if rc != 0 {
                        log::warn!("wifi: scan request rejected ({rc})");
                    } else {
                        log::info!("wifi: scan requested");
                        display::wifi_scan_requested(&domain);
                    }
                }
                display::Nav::OpenWifi => {
                    let results = collect_wifi_scan();
                    if !results.is_empty() {
                        display::set_wifi_scan(results);
                    }
                    display::navigate(&domain, display::Nav::OpenWifi);
                }
                _ => display::navigate(&domain, nav),
            }
        }
        if unsafe { ha_wifi_scan_ready() } != 0 {
            let results = collect_wifi_scan();
            log::info!("wifi: scan result {} networks", results.len());
            display::wifi_scan_done(&domain, results);
            unsafe { ha_wifi_scan_clear() };
        }
        while let Some(update) = system::take() {
            match update {
                system::Update::Location(location) => apply_system_location(&mut domain, location),
                system::Update::Weather(weather) => {
                    let changed = last_weather != Some(weather.condition);
                    last_weather = Some(weather.condition);
                    apply_weather(&mut domain, weather);
                    if changed {
                        let mut actions = ha_automation::Actions::new();
                        automation.on_event(
                            ENTITY_SYSTEM,
                            EVENT_WEATHER_CHANGED,
                            &DomainState(&domain),
                            &mut actions,
                        );
                        submit_automation(
                            &mut executor,
                            &mut command_core,
                            &mut bindings,
                            &actions,
                        );
                    }
                }
            }
        }
        while let Some(state) = bridge::take_state() {
            let entity = state.entity;
            let property = state.property;
            let value = ha_automation::value_to_f64(&state.value);
            apply_state(&mut domain, &mut command_core, state);
            if let Some(value) = value {
                let mut actions = ha_automation::Actions::new();
                automation.on_state(
                    entity,
                    property,
                    value,
                    &DomainState(&domain),
                    &mut actions,
                );
                submit_automation(&mut executor, &mut command_core, &mut bindings, &actions);
            }
        }
        while let Some((entity, capabilities)) = bridge::take_capabilities() {
            apply_capabilities(&mut domain, entity, capabilities);
        }
        while let Some(event) = bridge::take_event() {
            let entity = event.entity;
            let event_id = event.event;
            publish_event(&mut domain, event);
            let mut actions = ha_automation::Actions::new();
            automation.on_event(entity, event_id, &DomainState(&domain), &mut actions);
            submit_automation(&mut executor, &mut command_core, &mut bindings, &actions);
        }
        executor.tick(&mut transport, now_ms());
        while let Some(result) = executor.take_result() {
            log::info!("exec op={} status={:?}", result.id, result.status);
            if let Some(status) = status_from_exec(result.status) {
                command_core.on_result(result.id as u64, status);
            }
            if let Some(context) = executor.take_context(result.id) {
                if matches!(context.kind, zigbee_exec::RequestKind::ZdoLeave) {
                    if result.status == zigbee_exec::Status::Confirmed {
                        store::clear_removed(context.device);
                        log::info!("registry: leave done eui={:016x}", context.device);
                    } else {
                        log::warn!(
                            "registry: leave failed eui={:016x} status={:?}; mark kept",
                            context.device,
                            result.status
                        );
                    }
                }
                if let Some(read) = result.read {
                    ingest_read(&context, read);
                }
            }
            interview::on_operation_result(&mut executor, result, now_ms());
            configure::on_result(&mut executor, result, now_ms());
        }
        interview::tick(&mut executor, now_ms());
        configure::tick(&mut executor, now_ms());
        if let Some((minutes, weekday)) = local_minute(&domain) {
            if last_minute != Some((minutes, weekday)) {
                if last_minute.is_none() {
                    log::info!("time: local clock ready (time rules use Location tz)");
                }
                last_minute = Some((minutes, weekday));
                display::set_clock((minutes / 60) as u8, (minutes % 60) as u8);
                publish_system_time(&mut domain, minutes, weekday);
                let mut actions = ha_automation::Actions::new();
                automation.on_minute(
                    minutes,
                    1u8 << weekday,
                    &DomainState(&domain),
                    &mut actions,
                );
                submit_automation(&mut executor, &mut command_core, &mut bindings, &actions);
            }
        }
        let now = now_ms();
        if now.saturating_sub(last_wifi_poll) >= 2000 {
            last_wifi_poll = now;
            let mut rssi = 0i32;
            let up = unsafe { ha_wifi_rssi(&mut rssi) } != 0;
            display::set_wifi(up.then_some(rssi as i16));
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn ingest_read(context: &zigbee_exec::Request, read: zigbee_exec::Payload) {
    let zigbee_exec::RequestKind::ReadAttributes { cluster, .. } = context.kind else {
        return;
    };
    let Ok(frame) =
        z2m_rs::AttributeFrameView::parse(cluster, context.endpoint, 0x0A, read.as_slice())
    else {
        log::warn!("read: malformed response cluster=0x{cluster:04x}");
        return;
    };
    let count = frames::ingest(context.device, context.endpoint, cluster, &frame);
    log::info!(
        "read: eui={:016x} ep={} cluster=0x{:04x} decoded={count}",
        context.device,
        context.endpoint,
        cluster
    );
}

fn ingest_command(event: &PocEvent) {
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
    let entity = zigbee_devices::zigbee_entity_id(event.extended_addr, event.endpoint);
    for update in decoded.updates() {
        log::info!("canonical command eui={:016x} {update:?}", event.extended_addr);
        bridge::push(entity, update);
    }
}

fn apply_capabilities(
    domain: &mut Domain,
    entity: u64,
    capabilities: ha_domain::canonical::CapabilitiesRecord,
) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else { return; };
    if let Err(error) = adapter.write_capabilities(entity, &capabilities) {
        log::error!("domain: capabilities {entity:016x} failed: {error:?}");
        return;
    }
    web::upsert(
        ha_web::entity::CAPABILITIES,
        &entity.to_le_bytes(),
        &capabilities.encode(),
    );
    drop(adapter);
    display::rebuild(domain);
}

fn apply_state(
    domain: &mut Domain,
    command_core: &mut CommandCore,
    state: zigbee_devices::StateUpdate,
) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: state adapter init failed");
        return;
    };
    match adapter.write_state(state.entity, state.property, state.value) {
        Ok(changed) => {
            command_core.on_reported(state.entity, state.property);
            log::info!(
                "domain: state entity={:016x} property={} changed={changed}",
                state.entity,
                state.property
            );
            display::on_state(state.entity, state.property, state.value);
            if changed {
                let key = StateKey {
                    entity: state.entity,
                    property: state.property,
                }
                .to_bytes();
                let record = StateRecord {
                    value: state.value,
                }
                .encode();
                web::upsert(ha_web::entity::PROPERTY_STATE, &key, &record);
            }
        }
        Err(error) => log::error!("domain: state write failed: {error:?}"),
    }
}

fn publish_event(domain: &mut Domain, event: bridge::CanonicalEvent) {
    let key = event.entity.to_le_bytes();
    let payload = event.event.to_le_bytes();
    match domain.payload_put(
        Some(FactTarget {
            entity: ENTITY_ENTITY,
            key: &key,
        }),
        FactMeta {
            source: SOURCE_ZIGBEE as u8,
            value: ha_domain::Value::NONE,
        },
        &payload,
    ) {
        Ok(reference) => {
            log::info!(
                "domain: event entity={:016x} id={} payload={reference}",
                event.entity,
                event.event
            );
            web::publish_event(event.entity, event.event);
        }
        Err(error) => log::error!("domain: event write failed: {error:?}"),
    }
}
fn start_wifi() {
    /* Until provisioning exists the known list lives in ha_wifi.c; the build-time
     * env pair only adds/overrides one entry. */
    let ssid = CString::new(option_env!("HA_WIFI_SSID").unwrap_or("")).expect("ssid");
    let password = CString::new(option_env!("HA_WIFI_PASSWORD").unwrap_or("")).expect("password");

    let err = unsafe { ha_wifi_start(ssid.as_ptr(), password.as_ptr()) };
    if err != 0 {
        log::error!("wifi start failed: {err}");
        return;
    }
    let mut last_ip: Option<String> = None;
    let mut time_started = false;
    loop {
        let mut buf = [0 as c_char; 32];
        if unsafe { ha_wifi_ip(buf.as_mut_ptr(), buf.len()) } != 0 {
            let ip = unsafe { CStr::from_ptr(buf.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            if last_ip.as_deref() != Some(ip.as_str()) {
                log::info!("wifi ip={ip}");
                if !time_started {
                    unsafe { ha_time_start() };
                    log::info!("time: SNTP started (UTC)");
                    time_started = true;
                }
                match std::net::TcpStream::connect((ip.trim(), 80)) {
                    Ok(mut stream) => {
                        let _ = stream.set_read_timeout(Some(Duration::from_secs(2)));
                        let _ = stream.write_all(b"GET / HTTP/1.0\r\n\r\n");
                        let mut response = [0u8; 16];
                        match stream.read(&mut response) {
                            Ok(n) if n > 0 => log::info!("web: self-check ok ({n} bytes)"),
                            other => log::error!("web: self-check read failed: {other:?}"),
                        }
                    }
                    Err(error) => log::error!("web: self-check connect failed: {error}"),
                }
                last_ip = Some(ip);
            }
        }
        std::thread::sleep(Duration::from_millis(500));
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

struct DomainState<'a>(&'a Domain);

impl ha_automation::StateSource for DomainState<'_> {
    fn state(&self, entity: u64, property: u32) -> Option<f64> {
        let key = StateKey { entity, property }.to_bytes();
        let mut bytes = [0u8; ha_domain::canonical::STATE_RECORD_LEN];
        self.0
            .entity_get(ha_domain::canonical::ENTITY_PROPERTY_STATE, &key, &mut bytes)
            .ok()?;
        let value = ha_domain::Value::from_bytes(&bytes)?;
        ha_automation::value_to_f64(&value)
    }
}

fn rebuild_rules(domain: &Domain, engine: &mut ha_automation::RuleEngine) {
    engine.clear();
    if let Err(error) = domain.entity_iter(ha_domain::canonical::ENTITY_AUTOMATION, |key, record| {
        if key.len() == 8 && record.len() == ha_domain::canonical::AUTOMATION_RECORD_LEN {
            let id = u64::from_le_bytes(key.try_into().expect("automation key"));
            engine.add_rule(id, record);
        }
    }) {
        log::warn!("automation: rebuild failed: {error:?}");
    }
    engine.build_index();
    log::info!(
        "automation: rules={} index={}",
        engine.rule_count(),
        engine.index_count()
    );
}

fn submit_automation(
    executor: &mut zigbee_exec::Executor,
    command_core: &mut CommandCore,
    bindings: &mut [zigbee_devices::Binding],
    actions: &ha_automation::Actions,
) {
    if actions.is_empty() {
        return;
    }
    for binding in bindings.iter_mut() {
        binding.confirmed = interview::confirmed(binding.eui);
    }
    for command in actions.iter() {
        let submitted = zigbee_devices::CommandRouter::new(executor, bindings).submit(
            command_core,
            command.entity,
            command.property,
            command.action,
            command.value,
            now_ms(),
        );
        match submitted {
            Ok(operation) => log::info!(
                "automation: entity={:016x} property={} op={operation}",
                command.entity,
                command.property
            ),
            Err(error) => log::warn!(
                "automation: command rejected entity={:016x} property={} error={error:?}",
                command.entity,
                command.property
            ),
        }
    }
}

/// Seed the singleton Location so the UI can edit it before the first save.
fn ensure_system_location(domain: &mut Domain) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let mut current = [0u8; LOCATION_RECORD_LEN];
    if domain.entity_get(ENTITY_LOCATION, &key, &mut current).is_ok() {
        return;
    }
    let record = LocationRecord::default().encode();
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: location table init failed");
        return;
    };
    match adapter.put_record(ENTITY_LOCATION, &key, &record) {
        Ok(changed) => {
            if changed {
                web::upsert(ha_web::entity::LOCATION, &key, &record);
            }
            log::info!("domain: location seeded");
        }
        Err(error) => log::warn!("domain: location seed failed: {error:?}"),
    }
}

/// Local wall-clock minute for automation Time triggers. A manual offset is
/// applied; auto timezone falls back to UTC until GeoIP exists.
fn display_relevant(entity: u32) -> bool {
    matches!(
        entity,
        ha_domain::canonical::ENTITY_GROUP
            | ha_domain::canonical::ENTITY_GROUP_ITEM
            | ha_domain::canonical::ENTITY_GROUP_AUTOMATION
            | ha_domain::canonical::ENTITY_ENTITY
            | ha_domain::canonical::ENTITY_CAPABILITIES
            | ha_domain::canonical::ENTITY_AUTOMATION
    )
}

fn collect_wifi_scan() -> Vec<(String, i8)> {
    let count = unsafe { ha_wifi_scan_count() };
    let mut results = Vec::new();
    for index in 0..count {
        let mut ssid = [0 as c_char; 33];
        let mut rssi = 0i32;
        let ok = unsafe { ha_wifi_scan_get(index, ssid.as_mut_ptr(), ssid.len(), &mut rssi) };
        if ok != 0 {
            let ssid = unsafe { CStr::from_ptr(ssid.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            results.push((ssid, rssi as i8));
        }
    }
    results
}

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
        Ok(changed) => {
            if changed {
                web::upsert(ha_web::entity::SETTINGS, &key, &record);
            }
            log::info!("domain: settings seeded");
        }
        Err(error) => log::warn!("domain: settings seed failed: {error:?}"),
    }
}

fn read_location(domain: &Domain) -> Option<LocationRecord> {
    let mut bytes = [0u8; LOCATION_RECORD_LEN];
    domain
        .entity_get(ENTITY_LOCATION, &SYSTEM_DEVICE_UID.to_le_bytes(), &mut bytes)
        .ok()?;
    LocationRecord::decode(&bytes)
}

fn location_name(location: &LocationRecord) -> String {
    let end = location
        .name
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(location.name.len());
    String::from_utf8_lossy(&location.name[..end]).into_owned()
}

fn apply_system_location(domain: &mut Domain, location: LocationRecord) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let record = location.encode();
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        return;
    };
    match adapter.put_record(ENTITY_LOCATION, &key, &record) {
        Ok(changed) => {
            if changed {
                web::upsert(ha_web::entity::LOCATION, &key, &record);
            }
        }
        Err(error) => log::warn!("system: location write failed: {error:?}"),
    }
    drop(adapter);
    display::set_location(&location_name(&location));
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
        return;
    }
    web::upsert(ha_web::entity::WEATHER, &key, &record);
    display::set_weather(
        weather.condition,
        f32::from(weather.temperature_c100) / 100.0,
        f32::from(weather.humidity_p100) / 100.0,
    );
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
        (
            PROPERTY_SYSTEM_TIME_STATUS,
            ha_domain::Value::u32(2),
        ),
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
        if domain
            .entity_put(ENTITY_PROPERTY_STATE, &key, &record, meta)
            .is_ok()
        {
            web::upsert(ha_web::entity::PROPERTY_STATE, &key, &record);
        }
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
        .entity_get(ENTITY_LOCATION, &SYSTEM_DEVICE_UID.to_le_bytes(), &mut bytes)
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

fn register_device(
    domain: &mut Domain,
    bindings: &mut Vec<zigbee_devices::Binding>,
    eui: u64,
    short: u16,
    profile: &'static z2m_rs::Profile,
) {
    let name = store::model(eui).unwrap_or_else(|| profile.id.to_string());
    let descriptor = store::descriptor(eui);
    let mut registered = false;
    if let Some(descriptor) = descriptor {
        for endpoint in descriptor.endpoints() {
            register_domain(domain, bindings, eui, short, endpoint.endpoint, profile, &name);
            registered = true;
        }
    }
    if !registered {
        register_domain(domain, bindings, eui, short, 1, profile, &name);
    }
}

fn register_domain(
    domain: &mut Domain,
    bindings: &mut Vec<zigbee_devices::Binding>,
    eui: u64,
    short: u16,
    endpoint: u8,
    profile: &'static z2m_rs::Profile,
    name: &str,
) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: adapter init failed");
        return;
    };
    let device = match adapter.ensure_device(eui, name) {
        Ok((device, _)) => device,
        Err(error) => {
            log::error!("domain: device {eui:016x} failed: {error:?}");
            return;
        }
    };
    let standard_endpoint = if profile.id == zigbee_devices::STANDARD_PROFILE_ID {
        crate::store::descriptor(eui).and_then(|descriptor| descriptor.endpoint(endpoint).copied())
    } else {
        None
    };
    let entity_result = match standard_endpoint {
        Some(descriptor) => adapter.ensure_standard_entity(eui, endpoint, name, &descriptor),
        None => adapter.ensure_entity(eui, endpoint, name, profile),
    };
    match entity_result {
        Ok((entity, changed)) => {
            if !bindings.iter().any(|binding| binding.entity == entity) {
                bindings.push(zigbee_devices::Binding {
                    entity,
                    eui,
                    short,
                    endpoint,
                    profile,
                    standard: standard_endpoint,
                    confirmed: interview::confirmed(eui),
                });
            }
            if let Some(record) = adapter.read_device(device) {
                web::upsert(
                    ha_web::entity::LOGICAL_DEVICE,
                    &device.to_le_bytes(),
                    &record,
                );
            }
            if let Some(record) = adapter.read_entity(entity) {
                web::upsert(ha_web::entity::ENTITY, &entity.to_le_bytes(), &record);
            }
            if let Some(caps) = adapter.read_capabilities(entity) {
                web::upsert(ha_web::entity::CAPABILITIES, &entity.to_le_bytes(), &caps);
            }
            log::info!(
                "domain: entity={entity:016x} eui={eui:016x} profile={} changed={changed}",
                profile.id
            );
        }
        Err(error) => log::error!("domain: entity {eui:016x} failed: {error:?}"),
    }
}


fn rename_entity(domain: &mut Domain, entity: u64, name: &str) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: adapter init failed");
        return;
    };
    match adapter.rename_entity(entity, name) {
        Ok(_) => {
            if let Some(record) = adapter.read_entity(entity) {
                web::upsert(ha_web::entity::ENTITY, &entity.to_le_bytes(), &record);
            }
            log::info!("domain: renamed entity={entity:016x} name={name}");
        }
        Err(error) => log::warn!("domain: rename entity={entity:016x} failed: {error:?}"),
    }
}

fn rename_device(domain: &mut Domain, device: u64, name: &str) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: adapter init failed");
        return;
    };
    match adapter.rename_device(device, name) {
        Ok(_) => {
            if let Some(record) = adapter.read_device(device) {
                web::upsert(
                    ha_web::entity::LOGICAL_DEVICE,
                    &device.to_le_bytes(),
                    &record,
                );
            }
            log::info!("domain: renamed device={device:016x} name={name}");
        }
        Err(error) => log::warn!("domain: rename device={device:016x} failed: {error:?}"),
    }
}

fn request_leave(
    executor: &mut zigbee_exec::Executor,
    eui: u64,
    short: u16,
    now: u64,
) -> bool {
    if executor.pending_leave(eui) {
        return true;
    }
    let request = zigbee_exec::Request {
        device: eui,
        short,
        endpoint: 0,
        sleepy: false,
        retry: zigbee_exec::RetryPolicy::NEVER,
        expiry_ms: 10_000,
        timeout_ms: 3_000,
        kind: zigbee_exec::RequestKind::ZdoLeave,
    };
    match executor.enqueue(request, now) {
        Ok(id) => {
            log::info!("leave queued eui={eui:016x} op={id}");
            true
        }
        Err(error) => {
            log::warn!("leave queue failed eui={eui:016x}: {error:?}");
            false
        }
    }
}

fn remove_device(
    domain: &mut Domain,
    bindings: &mut Vec<zigbee_devices::Binding>,
    executor: &mut zigbee_exec::Executor,
    device: u64,
) {
    let mut sink = StoreSink;
    let Ok(mut adapter) = DomainAdapter::with_sink(domain, &mut sink) else {
        log::error!("domain: adapter init failed");
        return;
    };
    let eui = bindings
        .iter()
        .find(|binding| zigbee_devices::zigbee_device_id(binding.eui) == device)
        .map(|binding| binding.eui)
        .or_else(|| {
            adapter.read_device(device).and_then(|bytes| {
                let native = std::str::from_utf8(&bytes[12..44])
                    .ok()?
                    .trim_end_matches('\0');
                u64::from_str_radix(native, 16).ok()
            })
        });
    let entities: Vec<u64> = match eui {
        Some(eui) => bindings
            .iter()
            .filter(|binding| binding.eui == eui)
            .map(|binding| binding.entity)
            .collect(),
        None => Vec::new(),
    };
    let short = eui
        .and_then(|eui| {
            bindings
                .iter()
                .find(|binding| binding.eui == eui)
                .map(|binding| binding.short)
        })
        .unwrap_or(0);
    if let Some(eui) = eui {
        if !store::mark_removed(eui) {
            log::error!("domain: remove {device:016x} aborted: removal mark failed");
            return;
        }
    }
    match adapter.remove_device_records(device) {
        Ok(count) => log::info!("domain: removed device={device:016x} records={count}"),
        Err(error) => {
            log::warn!("domain: remove device={device:016x} failed: {error:?}");
            return;
        }
    }
    drop(adapter);
    for entity in entities {
        web::remove_entity(entity);
    }
    web::remove(ha_web::entity::LOGICAL_DEVICE, &device.to_le_bytes());
    if let Some(eui) = eui {
        interview::forget(eui);
        configure::forget(eui);
        if !request_leave(executor, eui, short, now_ms()) {
            log::warn!("domain: leave for {eui:016x} will retry on announce");
        }
        bindings.retain(|binding| binding.eui != eui);
    }
    log::info!("domain: removed device={device:016x}");
}

fn ensure_interview(executor: &mut zigbee_exec::Executor, eui: u64, short: u16) {
    if eui == 0 {
        return;
    }
    if store::is_removed(eui) {
        log::info!("registry: {eui:016x} marked removed; leave instead of interview");
        let _ = request_leave(executor, eui, short, now_ms());
        return;
    }
    if let Some(profile) = store::lookup(eui) {
        if interview::selected_profile(eui).is_none() {
            log::info!("registry: restored profile={} (no interview)", profile.id);
            interview::restore(eui, profile);
        }
    }
    if interview::selected_profile(eui).is_some() && store::descriptor(eui).is_some() {
        configure::ensure(executor, eui, short, now_ms());
        return;
    }
    interview::start(executor, (eui, short), now_ms());
}
