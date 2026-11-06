//! Zigbee adapter service: owns the executor, command core, bindings, radio
//! requests and the adapter-private interview/configure lifecycle.
//!
//! Domain only registers the command discriminator; its callback calls
//! `State::submit` synchronously, so `COMMAND_SENT` is written only after
//! validation and adapter queue acceptance.

use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use ha_domain::canonical::{
    CanonicalCommand, ENTITY_CAPABILITIES, ENTITY_ENTITY, SOURCE_UI, SOURCE_ZIGBEE,
};
use ha_domain::{DomainHandle, FactMeta, FactTarget, Value};
use zigbee_devices::{CommandCore, CommandRouter, DomainAdapter};

use crate::{configure, exec, frames, interview, removal_queue::RemovalQueue, store};

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

#[derive(Clone, Copy)]
struct CanonicalEvent {
    entity: u64,
    event: u32,
}

struct Ingest<'a> {
    domain: &'a DomainHandle,
    reported: &'a mut Vec<(u64, u32, Value)>,
    confirmations: &'a mut Vec<configure::Confirmation>,
}

impl frames::IngestSink for Ingest<'_> {
    fn update(&mut self, entity: u64, update: &z2m_rs::Update) {
        let (states, event) = zigbee_devices::canonical_states(entity, update);
        for state in states.into_iter().flatten() {
            apply_state(self.domain, self.reported, state);
        }
        if let Some(event_id) = event {
            publish_event(
                self.domain,
                CanonicalEvent {
                    entity,
                    event: event_id,
                },
            );
        }
    }

    fn capabilities(
        &mut self,
        entity: u64,
        capabilities: ha_domain::canonical::CapabilitiesRecord,
    ) {
        apply_capabilities(self.domain, entity, capabilities);
    }

    fn confirmed(&mut self, eui: u64, endpoint: u8, cluster: u16, attribute: u16) {
        self.confirmations.push((eui, endpoint, cluster, attribute));
    }
}

extern "C" {
    fn zb_poc_poll(event: *mut PocEvent) -> i32;
}

pub struct State {
    executor: zigbee_exec::Executor,
    command_core: CommandCore,
    bindings: Vec<zigbee_devices::Binding>,
}

impl State {
    pub fn new() -> Self {
        Self {
            executor: zigbee_exec::Executor::new(),
            command_core: CommandCore::new(),
            bindings: Vec::new(),
        }
    }

    pub fn confirm_binding(&mut self, eui: u64, endpoint: u8, cluster: u16, attribute: u16) {
        for binding in self
            .bindings
            .iter_mut()
            .filter(|binding| binding.eui == eui && binding.endpoint == endpoint)
        {
            binding.confirm(cluster, attribute);
        }
    }

    pub fn submit(&mut self, command: CanonicalCommand) -> Result<(), ha_domain::Error> {
        CommandRouter::new(&mut self.executor, &self.bindings)
            .submit(
                &mut self.command_core,
                command.entity,
                command.property,
                command.action,
                command.value,
                now_ms(),
            )
            .map(|_| ())
    }
}

pub struct Handle {
    requests: SyncSender<Request>,
}

enum Request {
    RemoveDevice(u64),
}

impl Handle {
    pub fn remove_device(&self, device: u64) -> Result<(), ha_domain::Error> {
        self.requests
            .try_send(Request::RemoveDevice(device))
            .map_err(|_| ha_domain::Error::NoSpace)
    }
}

static REMOVALS: Mutex<RemovalQueue> = Mutex::new(RemovalQueue::new());

pub fn start(domain: DomainHandle) -> (Handle, Arc<Mutex<State>>) {
    let (tx, rx) = sync_channel(8);
    let (state_tx, state_rx) = sync_channel(1);
    std::thread::Builder::new()
        .name("zigbee-service".into())
        .stack_size(65536)
        .spawn(move || {
            let state = Arc::new(Mutex::new(State::new()));
            ha_device::register_remover(SOURCE_ZIGBEE, zigbee_remover).expect("zigbee remover");
            restore_devices(&domain, &state);
            state_tx
                .send(Arc::clone(&state))
                .expect("publish zigbee state");
            run(domain, state, rx);
        })
        .expect("zigbee service thread");
    let state = state_rx.recv().expect("zigbee state");
    (Handle { requests: tx }, state)
}

fn zigbee_remover(native: &[u8]) -> Result<ha_device::NativeRemoval, ha_domain::Error> {
    let text = std::str::from_utf8(native).map_err(|_| ha_domain::Error::InvalidArg)?;
    let eui = u64::from_str_radix(text, 16).map_err(|_| ha_domain::Error::InvalidArg)?;
    store::mark_removed(eui)?;
    REMOVALS.lock().unwrap().try_push(eui)?;
    interview::forget(eui);
    configure::forget(eui);
    Ok(ha_device::NativeRemoval::Removed)
}

fn run(domain: DomainHandle, state: Arc<Mutex<State>>, requests: Receiver<Request>) {
    let mut transport = exec::EspTransport;
    loop {
        while let Ok(Request::RemoveDevice(device)) = requests.try_recv() {
            remove_device(&domain, &state, device);
        }

        let mut event = PocEvent::default();
        let polled = unsafe { zb_poc_poll(&mut event) } != 0;
        if polled {
            if event.extended_addr == 0 {
                if let Some(binding) = state
                    .lock()
                    .unwrap()
                    .bindings
                    .iter()
                    .find(|binding| binding.short == event.short_addr)
                {
                    event.extended_addr = binding.eui;
                }
            }
            {
                let mut state = state.lock().unwrap();
                state.executor.device_awake(event.short_addr);
                zigbee_devices::update_binding_short(
                    &mut state.bindings,
                    event.extended_addr,
                    event.short_addr,
                );
            }
            ensure_interview(&state, event.extended_addr, event.short_addr);
        }

        let mut reported = Vec::new();
        let mut confirmations = Vec::new();
        {
            let mut ingest = Ingest {
                domain: &domain,
                reported: &mut reported,
                confirmations: &mut confirmations,
            };
            if polled {
                ingest_command(&event, &mut ingest);
            }
            frames::process_pending(&mut ingest);
        }
        if !reported.is_empty() || !confirmations.is_empty() {
            let mut state = state.lock().unwrap();
            for (eui, endpoint, cluster, attribute) in confirmations {
                state.confirm_binding(eui, endpoint, cluster, attribute);
            }
            for (entity, property, value) in reported {
                state.command_core.on_reported(entity, property, value);
            }
        }

        if let Some(device) = frames::take_last_device() {
            {
                let mut state = state.lock().unwrap();
                state.executor.device_awake(device.1);
                zigbee_devices::update_binding_short(&mut state.bindings, device.0, device.1);
            }
            ensure_interview(&state, device.0, device.1);
        }
        while let Some((eui, short)) = interview::next_device() {
            ensure_interview(&state, eui, short);
        }
        while let Some((eui, short, profile)) = interview::take_identified() {
            register_device(&domain, &state, eui, short, profile);
        }

        let mut results = Vec::new();
        {
            let mut state = state.lock().unwrap();
            let now = now_ms();
            state.executor.tick(&mut transport, now);
            while let Some(result) = state.executor.take_result() {
                log::info!("exec op={} status={:?}", result.id, result.status);
                if let Some(status) = zigbee_devices::status_from_exec(result.status) {
                    state.command_core.on_result(result.id as u64, status);
                }
                let context = state.executor.take_context(result.id);
                results.push((result, context));
            }
        }
        for (result, context) in results {
            if let Some(context) = context {
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
                    let mut reported = Vec::new();
                    let mut confirmations = Vec::new();
                    {
                        let mut ingest = Ingest {
                            domain: &domain,
                            reported: &mut reported,
                            confirmations: &mut confirmations,
                        };
                        ingest_read(&context, read, &mut ingest);
                    }
                    if !reported.is_empty() || !confirmations.is_empty() {
                        let mut state = state.lock().unwrap();
                        for (eui, endpoint, cluster, attribute) in confirmations {
                            state.confirm_binding(eui, endpoint, cluster, attribute);
                        }
                        for (entity, property, value) in reported {
                            state.command_core.on_reported(entity, property, value);
                        }
                    }
                }
            }
            {
                let mut state = state.lock().unwrap();
                interview::on_operation_result(&mut state.executor, result, now_ms());
                for (eui, endpoint, cluster, attribute) in
                    configure::on_result(&mut state.executor, result, now_ms())
                {
                    state.confirm_binding(eui, endpoint, cluster, attribute);
                }
            }
        }
        {
            let mut state = state.lock().unwrap();
            interview::tick(&mut state.executor, now_ms());
            configure::tick(&mut state.executor, now_ms());
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn restore_devices(domain: &DomainHandle, state: &Arc<Mutex<State>>) {
    let (restored, restored_count) = store::device_addresses();
    for (eui, short) in restored.into_iter().take(restored_count) {
        if let Some(profile) = store::lookup(eui) {
            log::info!("registry: restored profile={} (no interview)", profile.id);
            interview::restore(eui, profile);
            if store::descriptor(eui).is_some() {
                register_device(domain, state, eui, short, profile);
                let mut state = state.lock().unwrap();
                configure::ensure(&mut state.executor, eui, short, now_ms());
            } else {
                let mut state = state.lock().unwrap();
                interview::start(&mut state.executor, (eui, short), now_ms());
            }
        }
    }
}

fn register_device(
    domain: &DomainHandle,
    state: &Arc<Mutex<State>>,
    eui: u64,
    short: u16,
    profile: &'static z2m_rs::Profile,
) {
    let name = store::model(eui).unwrap_or_else(|| profile.id.to_string());
    let descriptor = store::descriptor(eui);
    let mut registered = false;
    if let Some(descriptor) = descriptor {
        for endpoint in descriptor.endpoints() {
            register_domain(domain, state, eui, short, endpoint.endpoint, profile, &name);
            registered = true;
        }
    }
    if !registered {
        register_domain(domain, state, eui, short, 1, profile, &name);
    }
}

fn register_domain(
    domain: &DomainHandle,
    state: &Arc<Mutex<State>>,
    eui: u64,
    short: u16,
    endpoint: u8,
    profile: &'static z2m_rs::Profile,
    name: &str,
) {
    let native = format!("{eui:016x}");
    let desc = ha_device::DeviceDesc {
        source: SOURCE_ZIGBEE,
        native_id: &native,
        parent_native_id: None,
        name,
    };
    let device = match domain
        .transaction(|transaction| ha_device::register(transaction, SOURCE_ZIGBEE as u8, &desc))
    {
        Ok(device) => device,
        Err(error) => {
            log::error!("device: register {eui:016x} failed: {error:?}");
            return;
        }
    };
    let standard_endpoint = if profile.id == zigbee_devices::STANDARD_PROFILE_ID {
        store::descriptor(eui).and_then(|descriptor| descriptor.endpoint(endpoint).copied())
    } else {
        None
    };
    let entity = zigbee_devices::zigbee_entity_id(eui, endpoint);
    let entity = match domain.transaction(|transaction| -> Result<(u64, bool), ha_domain::Error> {
        let changed =
            ha_device::ensure_entity(transaction, SOURCE_ZIGBEE as u8, device, entity, name)?;
        let caps_changed = match standard_endpoint {
            Some(descriptor) => {
                if transaction
                    .entity_meta(ENTITY_CAPABILITIES, &entity.to_le_bytes())
                    .is_ok()
                {
                    false
                } else {
                    let caps = zigbee_devices::standard::capabilities_confirmed(
                        &descriptor,
                        &zigbee_devices::standard::Confirmed::new(),
                    );
                    ha_device::set_capabilities(transaction, SOURCE_ZIGBEE as u8, entity, &caps)?
                }
            }
            None => {
                let caps = zigbee_devices::profile_capabilities(profile);
                ha_device::set_capabilities(transaction, SOURCE_ZIGBEE as u8, entity, &caps)?
            }
        };
        Ok((entity, changed || caps_changed))
    }) {
        Ok((entity, changed)) => {
            log::info!(
                "domain: entity={entity:016x} eui={eui:016x} profile={} changed={changed}",
                profile.id
            );
            entity
        }
        Err(error) => {
            log::error!("domain: entity {eui:016x} failed: {error:?}");
            return;
        }
    };
    match domain.transaction(|transaction| {
        ha_device::bind_entity(transaction, SOURCE_ZIGBEE as u8, device, entity)
    }) {
        Ok(_) => {}
        Err(error) => {
            log::error!("device: bind entity={entity:016x} failed: {error:?}");
            return;
        }
    }
    let mut state = state.lock().unwrap();
    if !state
        .bindings
        .iter()
        .any(|binding| binding.entity == entity)
    {
        state.bindings.push(zigbee_devices::Binding {
            entity,
            eui,
            short,
            endpoint,
            profile,
            standard: standard_endpoint,
            confirmed: zigbee_devices::Confirmed::new(),
        });
    }
}

fn request_leave(state: &Arc<Mutex<State>>, eui: u64, short: u16, now: u64) -> bool {
    let mut state = state.lock().unwrap();
    if state.executor.pending_leave(eui) {
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
    match state.executor.enqueue(request, now) {
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

fn remove_device(domain: &DomainHandle, state: &Arc<Mutex<State>>, device: u64) {
    let plan = match ha_device::prepare_remove(domain, device) {
        Ok(plan) => plan,
        Err(error) => {
            log::warn!("device: prepare removal device={device:016x} failed: {error:?}");
            return;
        }
    };
    let mut ticket = match ha_device::invoke_remover(plan) {
        Ok(ticket) => ticket,
        Err(error) => {
            log::warn!("device: remover device={device:016x} failed: {error:?}");
            return;
        }
    };
    match ha_device::remove_canonical(domain, SOURCE_UI as u8, &mut ticket) {
        Ok(count) => log::info!("device: removed device={device:016x} records={count}"),
        Err(error) => {
            log::warn!("device: canonical removal device={device:016x} failed: {error:?}");
            return;
        }
    }
    let (removed, count) = REMOVALS.lock().unwrap().take_all();
    for eui in removed[..count].iter().copied() {
        let short = {
            let state = state.lock().unwrap();
            state
                .bindings
                .iter()
                .find(|binding| binding.eui == eui)
                .map(|binding| binding.short)
                .unwrap_or(0)
        };
        if !request_leave(state, eui, short, now_ms()) {
            log::warn!("device: leave for {eui:016x} will retry on announce");
        }
        state
            .lock()
            .unwrap()
            .bindings
            .retain(|binding| binding.eui != eui);
    }
}

fn ensure_interview(state: &Arc<Mutex<State>>, eui: u64, short: u16) {
    if eui == 0 {
        return;
    }
    if store::is_removed(eui) {
        log::info!("registry: {eui:016x} marked removed; leave instead of interview");
        let _ = request_leave(state, eui, short, now_ms());
        return;
    }
    if let Some(profile) = store::lookup(eui) {
        if interview::selected_profile(eui).is_none() {
            log::info!("registry: restored profile={} (no interview)", profile.id);
            interview::restore(eui, profile);
        }
    }
    let mut state = state.lock().unwrap();
    if interview::selected_profile(eui).is_some() && store::descriptor(eui).is_some() {
        configure::ensure(&mut state.executor, eui, short, now_ms());
        return;
    }
    interview::start(&mut state.executor, (eui, short), now_ms());
}

fn ingest_read(
    context: &zigbee_exec::Request,
    read: zigbee_exec::Payload,
    sink: &mut dyn frames::IngestSink,
) {
    let zigbee_exec::RequestKind::ReadAttributes { cluster, .. } = context.kind else {
        return;
    };
    let Ok(frame) =
        z2m_rs::AttributeFrameView::parse(cluster, context.endpoint, 0x0A, read.as_slice())
    else {
        log::warn!("read: malformed response cluster=0x{cluster:04x}");
        return;
    };
    let count = frames::ingest(context.device, context.endpoint, cluster, &frame, sink);
    log::info!(
        "read: eui={:016x} ep={} cluster=0x{:04x} decoded={count}",
        context.device,
        context.endpoint,
        cluster
    );
}

fn ingest_command(event: &PocEvent, sink: &mut dyn frames::IngestSink) {
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
        log::info!(
            "canonical command eui={:016x} {update:?}",
            event.extended_addr
        );
        sink.update(entity, update);
    }
}

fn apply_capabilities(
    domain: &DomainHandle,
    entity: u64,
    capabilities: ha_domain::canonical::CapabilitiesRecord,
) {
    let result = domain.transaction(|transaction| {
        ha_device::set_capabilities(transaction, SOURCE_ZIGBEE as u8, entity, &capabilities)
    });
    match result {
        Ok(_) => {}
        Err(error) => {
            log::error!("domain: capabilities {entity:016x} failed: {error:?}");
        }
    }
}

fn apply_state(
    domain: &DomainHandle,
    reported: &mut Vec<(u64, u32, Value)>,
    state: zigbee_devices::StateUpdate,
) {
    match DomainAdapter::write_state(domain, state.entity, state.property, state.value) {
        Ok(changed) => {
            reported.push((state.entity, state.property, state.value));
            if changed {
                log::info!(
                    "domain: state entity={:016x} property={} changed=true",
                    state.entity,
                    state.property
                );
            }
        }
        Err(error) => log::error!("domain: state write failed: {error:?}"),
    }
}

fn publish_event(domain: &DomainHandle, event: CanonicalEvent) {
    let key = event.entity.to_le_bytes();
    let mut payload = [0u8; 24];
    payload[0..4].copy_from_slice(&event.event.to_le_bytes());
    payload[8..16].copy_from_slice(&key);
    match domain.payload_put(
        Some(FactTarget {
            entity: ENTITY_ENTITY,
            key: &key,
        }),
        FactMeta {
            source: SOURCE_ZIGBEE as u8,
            value: Value::u32(event.event),
        },
        &payload,
    ) {
        Ok(reference) => log::info!(
            "domain: event entity={:016x} id={} payload={reference}",
            event.entity,
            event.event
        ),
        Err(error) => log::error!("domain: event write failed: {error:?}"),
    }
}

fn now_ms() -> u64 {
    (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000).max(0) as u64
}
