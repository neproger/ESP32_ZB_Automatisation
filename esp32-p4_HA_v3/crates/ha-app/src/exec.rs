//! Espressif SDK transport for `zigbee-exec`.
//!
//! The C bridge owns the SDK calls (with `esp_zigbee_lock_acquire`) and reports
//! AF confirms, ZCL/default responses and ZDO results back here. The operation id
//! (not a Rust pointer) is the SDK callback context, so a late callback can never
//! complete a reused object.

use std::sync::Mutex;

use zigbee_exec::{
    ClusterList, EndpointList, Event, Payload, Request, RequestKind, SendError, SimpleDescriptor,
    Transport,
};

const MAX_EVENTS: usize = 16;

struct EventRing {
    items: [Option<Event>; MAX_EVENTS],
    head: usize,
    tail: usize,
    dropped: u32,
}

impl EventRing {
    const fn new() -> Self {
        Self {
            items: [None; MAX_EVENTS],
            head: 0,
            tail: 0,
            dropped: 0,
        }
    }

    fn push(&mut self, event: Event) {
        if self.tail - self.head >= MAX_EVENTS {
            self.dropped = self.dropped.saturating_add(1);
            return;
        }
        self.items[self.tail % MAX_EVENTS] = Some(event);
        self.tail += 1;
    }

    fn pop(&mut self) -> Option<Event> {
        if self.head == self.tail {
            return None;
        }
        let event = self.items[self.head % MAX_EVENTS].take();
        self.head += 1;
        event
    }
}

static EVENTS: Mutex<EventRing> = Mutex::new(EventRing::new());

unsafe extern "C" fn on_confirm(op: u32, tsn: u8, success: u8) {
    EVENTS.lock().unwrap().push(Event::Confirm {
        op,
        tsn,
        success: success != 0,
    });
}

unsafe extern "C" fn on_response(
    src_short: u16,
    src_ep: u8,
    cluster: u16,
    command: u8,
    tsn: u8,
    rsp_to_cmd: i16,
    status: i16,
    read: *const u8,
    read_len: u16,
) {
    let read = if read.is_null() || read_len == 0 {
        None
    } else {
        let bytes = unsafe { std::slice::from_raw_parts(read, usize::from(read_len)) };
        match Payload::new(bytes) {
            Ok(payload) => Some(payload),
            Err(error) => {
                log::warn!("read payload rejected: {read_len} bytes, {error:?}");
                None
            }
        }
    };
    // A payload we could not copy must fail the read, never confirm an empty one.
    let status = if read.is_none() && read_len != 0 {
        Some(0x89)
    } else {
        (status >= 0).then_some(status as u8)
    };
    EVENTS.lock().unwrap().push(Event::ZclResponse {
        src: src_short,
        endpoint: src_ep,
        cluster,
        command,
        tsn,
        rsp_to_cmd: (rsp_to_cmd >= 0).then_some(rsp_to_cmd as u8),
        status,
        read,
    });
}

unsafe extern "C" fn on_zdo_active_ep(op: u32, count: u8, endpoints: *const u8) {
    let mut list = EndpointList::new();
    if !endpoints.is_null() {
        for index in 0..usize::from(count).min(8) {
            let endpoint = unsafe { *endpoints.add(index) };
            let _ = list.push(endpoint);
        }
    }
    EVENTS.lock().unwrap().push(Event::ZdoActiveEndpoints { op, endpoints: list });
}

unsafe extern "C" fn on_zdo_simple_desc(
    op: u32,
    endpoint: u8,
    profile_id: u16,
    device_id: u16,
    server: *const u16,
    server_count: u8,
    client: *const u16,
    client_count: u8,
) {
    let mut server_clusters = ClusterList::new();
    let mut client_clusters = ClusterList::new();
    if !server.is_null() {
        for index in 0..usize::from(server_count).min(16) {
            let _ = server_clusters.push(unsafe { *server.add(index) });
        }
    }
    if !client.is_null() {
        for index in 0..usize::from(client_count).min(16) {
            let _ = client_clusters.push(unsafe { *client.add(index) });
        }
    }
    EVENTS.lock().unwrap().push(Event::ZdoSimpleDescriptor {
        op,
        descriptor: SimpleDescriptor { endpoint, profile_id, device_id, server_clusters, client_clusters },
    });
}

unsafe extern "C" fn on_zdo(op: u32, success: u8) {
    EVENTS.lock().unwrap().push(Event::ZdoResult {
        op,
        success: success != 0,
    });
}

extern "C" {
    fn zb_exec_set_callbacks(
        confirm: Option<unsafe extern "C" fn(u32, u8, u8)>,
        response: Option<
            unsafe extern "C" fn(u16, u8, u16, u8, u8, i16, i16, *const u8, u16),
        >,
        zdo: Option<unsafe extern "C" fn(u32, u8)>,
        active_ep: Option<unsafe extern "C" fn(u32, u8, *const u8)>,
        simple_desc: Option<
            unsafe extern "C" fn(u32, u8, u16, u16, *const u16, u8, *const u16, u8),
        >,
    );
    fn zb_exec_read(
        op: u32,
        eui64: u64,
        short: u16,
        ep: u8,
        cluster: u16,
        attributes: *const u16,
        count: u8,
    ) -> i32;
    #[allow(clippy::too_many_arguments)]
    fn zb_exec_config_report(
        op: u32,
        eui64: u64,
        short: u16,
        ep: u8,
        cluster: u16,
        attr: u16,
        attr_type: u8,
        min: u16,
        max: u16,
        change: u8,
    ) -> i32;
    fn zb_exec_on_off(op: u32, eui64: u64, short: u16, ep: u8, on: u8) -> i32;
    #[allow(clippy::too_many_arguments)]
    fn zb_exec_send_zcl(
        op: u32,
        eui64: u64,
        short: u16,
        ep: u8,
        cluster: u16,
        command: u8,
        manuf_code: u16,
        frame_control: u8,
        payload: *const u8,
        len: u16,
    ) -> i32;
    fn zb_exec_bind(
        op: u32,
        eui64: u64,
        short: u16,
        src_ep: u8,
        cluster: u16,
        dst_ep: u8,
    ) -> i32;
    fn zb_exec_active_ep(op: u32, eui64: u64, short: u16) -> i32;
    fn zb_exec_simple_desc(op: u32, eui64: u64, short: u16, ep: u8) -> i32;
}

pub fn install() {
    unsafe {
        zb_exec_set_callbacks(
            Some(on_confirm),
            Some(on_response),
            Some(on_zdo),
            Some(on_zdo_active_ep),
            Some(on_zdo_simple_desc),
        )
    };
    log::info!("executor callbacks installed");
}

pub struct EspTransport;

impl Transport for EspTransport {
    fn send(&mut self, op: u32, request: &Request) -> Result<(), SendError> {
        let code = match request.kind {
            RequestKind::ReadAttributes {
                cluster,
                attributes,
            } => unsafe {
                let attributes = attributes.as_slice();
                zb_exec_read(
                    op,
                    request.device,
                    request.short,
                    request.endpoint,
                    cluster,
                    attributes.as_ptr(),
                    attributes.len() as u8,
                )
            },
            RequestKind::Zcl {
                cluster: 0x0006,
                command: 0x01,
                ..
            } => unsafe {
                zb_exec_on_off(op, request.device, request.short, request.endpoint, 1)
            },
            RequestKind::Zcl {
                cluster: 0x0006,
                command: 0x00,
                ..
            } => unsafe {
                zb_exec_on_off(op, request.device, request.short, request.endpoint, 0)
            },
            RequestKind::SendZcl {
                cluster,
                command,
                payload,
                manufacturer,
                frame_control,
                ..
            } => unsafe {
                let bytes = payload.as_slice();
                zb_exec_send_zcl(
                    op,
                    request.device,
                    request.short,
                    request.endpoint,
                    cluster,
                    command,
                    manufacturer.unwrap_or(0),
                    frame_control,
                    bytes.as_ptr(),
                    bytes.len() as u16,
                )
            },
            RequestKind::ConfigureReporting {
                cluster,
                attribute,
                attribute_type,
                min,
                max,
                change,
            } => unsafe {
                zb_exec_config_report(
                    op,
                    request.device,
                    request.short,
                    request.endpoint,
                    cluster,
                    attribute,
                    attribute_type,
                    min,
                    max,
                    change,
                )
            },
            RequestKind::ZdoActiveEndpoints => unsafe {
                zb_exec_active_ep(op, request.device, request.short)
            },
            RequestKind::ZdoSimpleDescriptor { endpoint } => unsafe {
                zb_exec_simple_desc(op, request.device, request.short, endpoint)
            },
            RequestKind::ZdoBind { cluster } => unsafe {
                zb_exec_bind(
                    op,
                    request.device,
                    request.short,
                    request.endpoint,
                    cluster,
                    1,
                )
            },
            _ => return Err(SendError::Unsupported),
        };
        if code == 0 {
            Ok(())
        } else {
            Err(SendError::Io)
        }
    }

    fn poll(&mut self) -> Option<Event> {
        EVENTS.lock().unwrap().pop()
    }
}

/// Sequential ConfigurePlan runner over `zigbee-exec`: no own waiters/retries.
/// It knows only request shapes, not models.
const MAX_CONFIG_STEPS: usize = 32;
static EMPTY_STEPS: &[z2m_rs::ConfigureStep] = &[];
static EMPTY_PLAN: z2m_rs::ConfigurePlan = z2m_rs::ConfigurePlan {
    steps: EMPTY_STEPS, timeout_ms: 5_000, retries: 2, version: 1,
};

pub struct ConfigRun {
    device: (u64, u16),
    endpoint: u8,
    definition: &'static str,
    plan: &'static z2m_rs::ConfigurePlan,
    steps: [Option<z2m_rs::ConfigureStep>; MAX_CONFIG_STEPS],
    step_count: usize,
    version: u16,
    timeout_ms: u32,
    retries: u8,
    next: usize,
    op: Option<u32>,
    pending: Option<zigbee_exec::OperationResult>,
    failed: bool,
    done: bool,
}

impl ConfigRun {
    pub fn start(
        exec: &mut zigbee_exec::Executor,
        device: (u64, u16),
        endpoint: u8,
        definition: &'static str,
        plan: &'static z2m_rs::ConfigurePlan,
        now: u64,
    ) -> Self {
        let mut run = Self {
            device,
            endpoint,
            definition,
            plan,
            steps: [None; MAX_CONFIG_STEPS],
            step_count: 0,
            version: plan.version,
            timeout_ms: plan.timeout_ms,
            retries: plan.retries,
            next: 0,
            op: None,
            pending: None,
            failed: false,
            done: false,
        };
        log::info!(
            "configure: start device={:016x} version={}",
            device.0,
            plan.version
        );
        run.schedule(exec, now);
        run
    }

    pub fn start_owned(
        exec: &mut zigbee_exec::Executor,
        device: (u64, u16),
        endpoint: u8,
        definition: &'static str,
        steps: &[z2m_rs::ConfigureStep],
        version: u16,
        now: u64,
    ) -> Self {
        let mut run = Self {
            device,
            endpoint,
            definition,
            plan: &EMPTY_PLAN,
            steps: [None; MAX_CONFIG_STEPS],
            step_count: steps.len().min(MAX_CONFIG_STEPS),
            version,
            timeout_ms: 5_000,
            retries: 2,
            next: 0,
            op: None,
            pending: None,
            failed: false,
            done: false,
        };
        for (slot, step) in run.steps.iter_mut().zip(steps.iter().copied()) {
            *slot = Some(step);
        }
        log::info!("configure: start standard device={:016x} steps={}", device.0, run.step_count);
        run.schedule(exec, now);
        run
    }

    pub fn eui(&self) -> u64 {
        self.device.0
    }

    pub fn definition(&self) -> &'static str {
        self.definition
    }

    pub fn version(&self) -> u16 {
        self.plan.version
    }

    pub fn current_step(&self) -> Option<z2m_rs::ConfigureStep> {
        if self.step_count > 0 {
            self.steps.get(self.next).and_then(|step| *step)
        } else {
            self.plan.steps.get(self.next).copied()
        }
    }

    pub fn accepts(&self, operation: u32) -> bool {
        self.op == Some(operation)
    }

    pub fn failed(&self) -> bool {
        self.failed
    }

    pub fn on_result(&mut self, result: zigbee_exec::OperationResult) {
        if self.op == Some(result.id) {
            self.pending = Some(result);
        }
    }

    /// Returns true when the plan is finished (done or failed).
    pub fn tick(&mut self, exec: &mut zigbee_exec::Executor, now: u64) -> bool {
        if self.done || self.failed {
            return true;
        }
        if let Some(result) = self.pending.take() {
            log::info!(
                "configure: step={} op={} status={:?}",
                self.next,
                result.id,
                result.status
            );
            match result.status {
                zigbee_exec::Status::Confirmed | zigbee_exec::Status::Acknowledged => {
                    self.next += 1;
                    self.op = None;
                }
                other => {
                    self.failed = true;
                    log::warn!("configure: step={} failed: {other:?}", self.next);
                    return true;
                }
            }
        }
        let count = if self.step_count > 0 { self.step_count } else { self.plan.steps.len() };
        if self.next >= count {
            self.done = true;
            log::info!("configure: done version={} (RAM)", self.version);
            return true;
        }
        if self.op.is_none() {
            self.schedule(exec, now);
        }
        self.done || self.failed
    }

    fn schedule(&mut self, exec: &mut zigbee_exec::Executor, now: u64) {
        let Some((endpoint, kind)) = self.kind() else {
            self.failed = true;
            log::warn!("configure: unsupported step={}", self.next);
            return;
        };
        let request = zigbee_exec::Request {
            device: self.device.0,
            short: self.device.1,
            endpoint,
            // Battery devices must not fail a plan because they slept.
            sleepy: true,
            retry: zigbee_exec::RetryPolicy {
                max_attempts: self.retries.clamp(1, 3),
            },
            expiry_ms: self.timeout_ms,
            timeout_ms: 3_000,
            kind,
        };
        match exec.enqueue(request, now) {
            Ok(id) => {
                self.op = Some(id);
                log::info!("configure: step={} op={id}", self.next);
            }
            Err(error) => {
                self.failed = true;
                log::warn!("configure: enqueue step={} failed: {error:?}", self.next);
            }
        }
    }

    fn kind(&self) -> Option<(u8, zigbee_exec::RequestKind)> {
        use z2m_rs::{ConfigureEndpoint, ConfigureStep};
        let step = if self.step_count > 0 { self.steps.get(self.next)?.as_ref()? } else { self.plan.steps.get(self.next)? };
        let endpoint = match step {
            ConfigureStep::Read { endpoint, .. }
            | ConfigureStep::Bind { endpoint, .. }
            | ConfigureStep::Report { endpoint, .. }
            | ConfigureStep::Write { endpoint, .. } => match endpoint {
                ConfigureEndpoint::Id(id) => *id,
                ConfigureEndpoint::First => self.endpoint,
            },
        };
        let kind = match step {
            ConfigureStep::Read {
                cluster, attributes, ..
            } => zigbee_exec::RequestKind::ReadAttributes {
                cluster: *cluster,
                attributes: zigbee_exec::AttributeList::new(attributes).ok()?,
            },
            ConfigureStep::Bind { cluster, .. } => {
                zigbee_exec::RequestKind::ZdoBind { cluster: *cluster }
            }
            ConfigureStep::Report {
                cluster,
                attribute,
                attribute_type,
                min,
                max,
                change,
                ..
            } => zigbee_exec::RequestKind::ConfigureReporting {
                cluster: *cluster,
                attribute: *attribute,
                attribute_type: *attribute_type,
                min: *min,
                max: *max,
                change: (*change).min(u32::from(u8::MAX)) as u8,
            },
            ConfigureStep::Write { .. } => return None,
        };
        Some((endpoint, kind))
    }
}