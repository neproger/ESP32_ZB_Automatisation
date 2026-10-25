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
    EntityRecord, LogicalDeviceRecord, StateKey, StateRecord, ENTITY_ENTITY, SOURCE_ZIGBEE,
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
            for binding in &mut bindings {
                binding.confirmed = interview::confirmed(binding.eui);
            }
            let submitted = zigbee_devices::CommandRouter::new(&mut executor, &bindings).submit(
                &mut command_core,
                request.entity,
                request.property,
                request.action,
                request.value,
                now_ms(),
            );
            match submitted {
                Ok(operation) => log::info!(
                    "command: entity={:016x} property={} op={}",
                    request.entity,
                    request.property,
                    operation
                ),
                Err(error) => log::warn!(
                    "command rejected: entity={:016x} property={} error={error:?}",
                    request.entity,
                    request.property
                ),
            }
        }
        while let Some(state) = bridge::take_state() {
            apply_state(&mut domain, &mut command_core, state);
        }
        while let Some((entity, capabilities)) = bridge::take_capabilities() {
            apply_capabilities(&mut domain, entity, capabilities);
        }
        while let Some(event) = bridge::take_event() {
            publish_event(&mut domain, event);
        }
        executor.tick(&mut transport, now_ms());
        while let Some(result) = executor.take_result() {
            log::info!("exec op={} status={:?}", result.id, result.status);
            if let Some(status) = status_from_exec(result.status) {
                command_core.on_result(result.id as u64, status);
            }
            if let Some(context) = executor.take_context(result.id) {
                if let Some(read) = result.read {
                    ingest_read(&context, read);
                }
            }
            interview::on_operation_result(&mut executor, result, now_ms());
            configure::on_result(&mut executor, result, now_ms());
        }
        interview::tick(&mut executor, now_ms());
        configure::tick(&mut executor, now_ms());
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
    for _ in 0..120 {
        let mut buf = [0 as c_char; 32];
        if unsafe { ha_wifi_ip(buf.as_mut_ptr(), buf.len()) } != 0 {
            let ip = unsafe { CStr::from_ptr(buf.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            log::info!("wifi ip={ip}");
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
            return;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    log::warn!("wifi: no IPv4 after 60 s");
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
            let native = format!("{eui:016x}");
            let device_record = LogicalDeviceRecord::new(SOURCE_ZIGBEE, 0, &native, name);
            web::upsert(
                ha_web::entity::LOGICAL_DEVICE,
                &device.to_le_bytes(),
                &device_record.encode(),
            );
            let entity_record = EntityRecord::new(device, name);
            web::upsert(
                ha_web::entity::ENTITY,
                &entity.to_le_bytes(),
                &entity_record.encode(),
            );
            let capabilities = match standard_endpoint {
                Some(descriptor) => zigbee_devices::standard::capabilities_confirmed(
                    &descriptor,
                    &interview::confirmed(eui),
                ),
                None => zigbee_devices::profile_capabilities(profile),
            };
            web::upsert(
                ha_web::entity::CAPABILITIES,
                &entity.to_le_bytes(),
                &capabilities.encode(),
            );
            log::info!(
                "domain: entity={entity:016x} eui={eui:016x} profile={} changed={changed}",
                profile.id
            );
        }
        Err(error) => log::error!("domain: entity {eui:016x} failed: {error:?}"),
    }
}


fn ensure_interview(executor: &mut zigbee_exec::Executor, eui: u64, short: u16) {
    if eui == 0 {
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
