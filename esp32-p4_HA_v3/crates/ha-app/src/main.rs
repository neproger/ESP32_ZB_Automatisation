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
    StateKey, StateRecord, ENTITY_ENTITY, SOURCE_ZIGBEE,
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
    fn ha_wifi_reconnect();
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
        while let Some((eui, short, profile)) = interview::take_identified() {
            register_device(&mut domain, &mut bindings, eui, short, profile);
        }
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
                    rename_entity(&mut domain, entity, &name)
                }
                intake::Request::RenameDevice { device, name } => {
                    rename_device(&mut domain, device, &name)
                }
                intake::Request::RemoveDevice { device } => {
                    remove_device(&mut domain, &mut bindings, &mut executor, device)
                }
                intake::Request::Upsert {
                    entity,
                    key,
                    record,
                } => {
                    let is_rule = entity == ha_domain::canonical::ENTITY_AUTOMATION;
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
                }
                intake::Request::Remove { entity, key } => {
                    let is_rule = entity == ha_domain::canonical::ENTITY_AUTOMATION;
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
        if let Some((minutes, weekday)) = utc_minute() {
            if last_minute != Some((minutes, weekday)) {
                if last_minute.is_none() {
                    log::info!("time: UTC clock ready (time rules use UTC)");
                }
                last_minute = Some((minutes, weekday));
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
    let mut last_ip: Option<String> = None;
    let mut time_started = false;
    let mut misses = 0u32;
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
            misses = 0;
        } else {
            // V2 watchdog: two 5 s misses without an address -> retry with backoff.
            misses += 1;
            if misses >= 10 {
                misses = 0;
                unsafe { ha_wifi_reconnect() };
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

fn utc_minute() -> Option<(u16, u8)> {
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .ok()?
        .as_secs();
    // Before SNTP the clock is at 1970; do not fire time rules at 00:00.
    if seconds < 1_600_000_000 {
        return None;
    }
    let weekday = ((seconds / 86_400 + 3) % 7) as u8;
    let minutes = ((seconds % 86_400) / 60) as u16;
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
