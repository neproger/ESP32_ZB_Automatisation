//! Espressif SDK transport for `zigbee-exec`.
//!
//! The C bridge owns the SDK calls (with `esp_zigbee_lock_acquire`) and reports
//! AF confirms, ZCL/default responses and ZDO results back here. The operation id
//! (not a Rust pointer) is the SDK callback context, so a late callback can never
//! complete a reused object.

use std::sync::Mutex;

use zigbee_exec::{Event, Payload, Request, RequestKind, SendError, Transport};

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
    );
    fn zb_exec_read(op: u32, eui64: u64, short: u16, ep: u8, cluster: u16, attr: u16) -> i32;
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
    fn zb_exec_bind(
        op: u32,
        eui64: u64,
        short: u16,
        src_ep: u8,
        cluster: u16,
        dst_ep: u8,
    ) -> i32;
}

pub fn install() {
    unsafe { zb_exec_set_callbacks(Some(on_confirm), Some(on_response), Some(on_zdo)) };
    log::info!("executor callbacks installed");
}

pub struct EspTransport;

impl Transport for EspTransport {
    fn send(&mut self, op: u32, request: &Request) -> Result<(), SendError> {
        let code = match request.kind {
            RequestKind::ReadAttributes { cluster, attribute } => unsafe {
                zb_exec_read(
                    op,
                    request.device,
                    request.short,
                    request.endpoint,
                    cluster,
                    attribute,
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

/// Minimal configure runner over `zigbee-exec`: sequential steps, no own waiters.
pub struct ConfigRun {
    device: (u64, u16),
    next: usize,
    op: Option<u32>,
    pending: Option<zigbee_exec::OperationResult>,
    failed: bool,
    done: bool,
}

impl ConfigRun {
    pub fn start(exec: &mut zigbee_exec::Executor, device: (u64, u16), now: u64) -> Self {
        let mut run = Self {
            device,
            next: 0,
            op: None,
            pending: None,
            failed: false,
            done: false,
        };
        log::info!("configure: start device={:016x}", device.0);
        run.schedule(exec, now);
        run
    }

    pub fn on_result(&mut self, result: zigbee_exec::OperationResult) {
        self.pending = Some(result);
    }

    pub fn tick(&mut self, exec: &mut zigbee_exec::Executor, now: u64) {
        if self.done || self.failed {
            return;
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
                    return;
                }
            }
        }
        if self.next >= 3 {
            self.done = true;
            log::info!("configure: done version=1 (RAM)");
            return;
        }
        if self.op.is_none() {
            self.schedule(exec, now);
        }
    }

    fn schedule(&mut self, exec: &mut zigbee_exec::Executor, now: u64) {
        let Some(kind) = self.kind() else {
            self.failed = true;
            return;
        };
        let request = zigbee_exec::Request {
            device: self.device.0,
            short: self.device.1,
            endpoint: 1,
            sleepy: false,
            retry: zigbee_exec::RetryPolicy::SAFE,
            expiry_ms: 10_000,
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

    fn kind(&self) -> Option<zigbee_exec::RequestKind> {
        Some(match self.next {
            0 => zigbee_exec::RequestKind::ZdoBind { cluster: 0x0006 },
            1 => zigbee_exec::RequestKind::ConfigureReporting {
                cluster: 0x0001,
                attribute: 0x0021,
                attribute_type: 0x20,
                min: 3600,
                max: 7200,
                change: 0,
            },
            2 => zigbee_exec::RequestKind::ReadAttributes {
                cluster: 0x0001,
                attribute: 0x0021,
            },
            _ => return None,
        })
    }
}