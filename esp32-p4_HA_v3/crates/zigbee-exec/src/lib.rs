//! Request Executor: reliable delivery of Zigbee operations.
//!
//! Ported model of `zigbee-herdsman` request handling: per device/endpoint
//! serialization, AF-confirm with the SDK-assigned TSN, response waiters keyed by
//! the full context `(short address, endpoint, cluster, command, tsn)`, bounded
//! early-response buffer (response before confirm), expiry, selective retry and
//! sleepy parking.
//!
//! It does not know profiles or devices: `z2m-rs` builds requests, the HA layer
//! schedules them, a transport (fake here, Espressif SDK later) sends them.
//!
//! Completion is defined per request, not as one fixed state chain:
//! * `Expect::None` finishes as `Acknowledged` on a successful AF confirm;
//! * `ZclResponse`/`DefaultResponse` finishes as `Confirmed` (or `Failed`);
//! * ZDO bind finishes on the SDK callback, without a TSN.
//! Acknowledged delivery is never treated as confirmed device state.

use std::collections::VecDeque;

pub const MAX_OPS: usize = 32;
pub const MAX_PAYLOAD: usize = 64;
pub const MAX_EARLY: usize = 8;
pub const MAX_RESULTS: usize = 32;
pub const MAX_ATTEMPTS: u8 = 3;
pub const EARLY_TTL_MS: u64 = 5_000;
pub const RETRY_BACKOFF_MS: u64 = 200;

/// Per-operation retry budget. Not every idempotent command is safe to repeat
/// (e.g. some Tuya writes); the profile/operation decides the policy.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RetryPolicy {
    pub max_attempts: u8,
}

impl RetryPolicy {
    /// Send once; an unknown outcome is never repeated.
    pub const NEVER: Self = Self { max_attempts: 1 };
    /// Standard safe operations (on/off, read, bind, reporting).
    pub const SAFE: Self = Self { max_attempts: 3 };
}

pub type OperationId = u32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Payload {
    bytes: [u8; MAX_PAYLOAD],
    len: u8,
}

impl Payload {
    pub fn new(data: &[u8]) -> Result<Self, ExecError> {
        if data.len() > MAX_PAYLOAD {
            return Err(ExecError::PayloadTooLarge);
        }
        let mut bytes = [0; MAX_PAYLOAD];
        bytes[..data.len()].copy_from_slice(data);
        Ok(Self {
            bytes,
            len: data.len() as u8,
        })
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.bytes[..usize::from(self.len)]
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Expect {
    /// Delivery confirmation only (AF confirm).
    None,
    /// ZCL response command (e.g. Read Attributes Response 0x01).
    ZclResponse { command: u8 },
    /// Default Response; matched by `rsp_to_cmd`.
    DefaultResponse { to_command: u8 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RequestKind {
    Zcl {
        cluster: u16,
        command: u8,
        payload: Payload,
        expect: Expect,
    },
    /// Read one attribute; the response is matched as a normal ZCL response.
    ReadAttributes { cluster: u16, attribute: u16 },
    /// Configure one reporting record; response 0x07 carries the per-record status.
    ConfigureReporting {
        cluster: u16,
        attribute: u16,
        attribute_type: u8,
        min: u16,
        max: u16,
        change: u8,
    },
    /// Completed by the SDK ZDO callback; no TSN waiter is used.
    ZdoBind { cluster: u16 },
}

#[derive(Clone, Copy, Debug)]
pub struct Request {
    /// Durable identity for the queue; current address for correlation.
    pub device: u64,
    pub short: u16,
    pub endpoint: u8,
    pub sleepy: bool,
    pub retry: RetryPolicy,
    pub expiry_ms: u32,
    pub timeout_ms: u32,
    pub kind: RequestKind,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Queued,
    Sent,
    Acknowledged,
    Confirmed,
    Timeout,
    Failed,
    Expired,
}

impl Status {
    pub fn is_final(self) -> bool {
        matches!(
            self,
            Status::Confirmed | Status::Timeout | Status::Failed | Status::Expired
        )
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OperationResult {
    pub id: OperationId,
    pub status: Status,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExecError {
    QueueFull,
    PayloadTooLarge,
    ResultQueueFull,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SendError {
    /// Transport is busy; try again on the next tick.
    Full,
    Unsupported,
    Io,
}

#[derive(Clone, Copy, Debug)]
pub enum Event {
    Confirm {
        op: OperationId,
        tsn: u8,
        success: bool,
    },
    ZclResponse {
        src: u16,
        endpoint: u8,
        cluster: u16,
        command: u8,
        tsn: u8,
        rsp_to_cmd: Option<u8>,
        status: Option<u8>,
    },
    ZdoResult {
        op: OperationId,
        success: bool,
    },
}

pub trait Transport {
    /// The executor passes `op` as the confirmation context (`user_ctx`).
    fn send(&mut self, op: OperationId, request: &Request) -> Result<(), SendError>;
    fn poll(&mut self) -> Option<Event>;
}

#[derive(Clone, Copy)]
struct Op {
    id: OperationId,
    seq: u32,
    request: Request,
    status: Status,
    attempts: u8,
    tsn: Option<u8>,
    sent_at: u64,
    deadline: u64,
    not_before: u64,
    parked: bool,
}

#[derive(Clone, Copy)]
struct Early {
    response: Event,
    at: u64,
}

pub struct Executor {
    ops: [Option<Op>; MAX_OPS],
    early: [Option<Early>; MAX_EARLY],
    results: VecDeque<OperationResult>,
    next_id: OperationId,
    next_seq: u32,
    early_dropped: u32,
    results_dropped: u32,
}

impl Default for Executor {
    fn default() -> Self {
        Self::new()
    }
}

impl Executor {
    pub fn new() -> Self {
        Self {
            ops: [None; MAX_OPS],
            early: [None; MAX_EARLY],
            results: VecDeque::with_capacity(MAX_RESULTS),
            next_id: 1,
            next_seq: 0,
            early_dropped: 0,
            results_dropped: 0,
        }
    }

    pub fn enqueue(&mut self, request: Request, now: u64) -> Result<OperationId, ExecError> {
        let index = self
            .ops
            .iter()
            .position(Option::is_none)
            .ok_or(ExecError::QueueFull)?;
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1).max(1);
        self.ops[index] = Some(Op {
            id,
            seq: self.next_seq,
            request,
            status: Status::Queued,
            attempts: 0,
            tsn: None,
            sent_at: 0,
            deadline: now + u64::from(request.expiry_ms),
            not_before: 0,
            parked: false,
        });
        self.next_seq = self.next_seq.wrapping_add(1);
        Ok(id)
    }

    pub fn status(&self, id: OperationId) -> Option<Status> {
        self.ops
            .iter()
            .flatten()
            .find(|op| op.id == id)
            .map(|op| op.status)
    }

    pub fn take_result(&mut self) -> Option<OperationResult> {
        self.results.pop_front()
    }

    pub fn early_dropped(&self) -> u32 {
        self.early_dropped
    }

    pub fn results_dropped(&self) -> u32 {
        self.results_dropped
    }

    /// A sleepy device sent/rejoined: parked operations may be retried.
    pub fn device_awake(&mut self, short: u16) {
        for op in self.ops.iter_mut().flatten() {
            if op.request.short == short && op.parked {
                op.parked = false;
                op.status = Status::Queued;
                op.tsn = None;
                op.not_before = 0;
            }
        }
    }

    pub fn tick<T: Transport>(&mut self, transport: &mut T, now: u64) {
        self.drain_events(transport, now);
        self.expire(now);
        self.handle_timeouts(now);
        self.send_queued(transport, now);
        self.prune_early(now);
    }

    fn drain_events<T: Transport>(&mut self, transport: &mut T, now: u64) {
        for _ in 0..MAX_OPS {
            let Some(event) = transport.poll() else {
                break;
            };
            match event {
                Event::Confirm { op, tsn, success } => self.on_confirm(op, tsn, success, now),
                Event::ZclResponse {
                    src,
                    endpoint,
                    cluster,
                    command,
                    tsn,
                    rsp_to_cmd,
                    status,
                } => self.on_response(
                    src,
                    endpoint,
                    cluster,
                    command,
                    tsn,
                    rsp_to_cmd,
                    status,
                    now,
                ),
                Event::ZdoResult { op, success } => self.on_zdo_result(op, success, now),
            }
        }
    }

    fn on_confirm(&mut self, id: OperationId, tsn: u8, success: bool, now: u64) {
        let Some(index) = self.find_index(id) else {
            return;
        };
        if !success {
            self.retry_or_finish(index, Status::Failed, now);
            return;
        }

        let expect = match self.ops[index].as_ref().map(|op| op.request.kind) {
            Some(RequestKind::Zcl { expect, .. }) => expect,
            Some(RequestKind::ReadAttributes { .. }) => Expect::ZclResponse { command: 0x01 },
            Some(RequestKind::ConfigureReporting { .. }) => Expect::ZclResponse { command: 0x07 },
            Some(RequestKind::ZdoBind { .. }) => {
                let op = self.ops[index].as_mut().unwrap();
                op.status = Status::Acknowledged;
                op.sent_at = now;
                return;
            }
            None => return,
        };

        if expect == Expect::None {
            self.finish(index, Status::Acknowledged);
            return;
        }

        {
            let op = self.ops[index].as_mut().unwrap();
            op.tsn = Some(tsn);
            op.status = Status::Acknowledged;
            op.sent_at = now;
        }
        if let Some(early) = self.take_early_for(index, tsn, now) {
            self.apply_response(index, early);
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn on_response(
        &mut self,
        src: u16,
        endpoint: u8,
        cluster: u16,
        command: u8,
        tsn: u8,
        rsp_to_cmd: Option<u8>,
        status: Option<u8>,
        now: u64,
    ) {
        let response = Event::ZclResponse {
            src,
            endpoint,
            cluster,
            command,
            tsn,
            rsp_to_cmd,
            status,
        };
        // Waiters know their TSN (confirm already arrived).
        let found = self.ops.iter().enumerate().find_map(|(index, op)| {
            let op = op.as_ref()?;
            if op.tsn != Some(tsn) || op.status != Status::Acknowledged {
                return None;
            }
            self.response_matches(op, &response).then_some(index)
        });
        if let Some(index) = found {
            self.apply_response(index, response);
            return;
        }
        self.buffer_early(response, now);
    }

    fn on_zdo_result(&mut self, id: OperationId, success: bool, now: u64) {
        let Some(index) = self.find_index(id) else {
            return;
        };
        let is_zdo = matches!(
            self.ops[index].as_ref().map(|op| op.request.kind),
            Some(RequestKind::ZdoBind { .. })
        );
        if !is_zdo {
            return;
        }
        let _ = now;
        if success {
            self.finish(index, Status::Confirmed);
        } else {
            self.retry_or_finish(index, Status::Failed, now);
        }
    }

    fn response_matches(&self, op: &Op, response: &Event) -> bool {
        let (req_cluster, expect) = match op.request.kind {
            RequestKind::Zcl {
                cluster, expect, ..
            } => (cluster, expect),
            RequestKind::ReadAttributes { cluster, .. } => (cluster, Expect::ZclResponse { command: 0x01 }),
            RequestKind::ConfigureReporting { cluster, .. } => (cluster, Expect::ZclResponse { command: 0x07 }),
            RequestKind::ZdoBind { .. } => return false,
        };
        let Event::ZclResponse {
            src,
            endpoint,
            cluster,
            command,
            rsp_to_cmd,
            ..
        } = *response
        else {
            return false;
        };
        if src != op.request.short || endpoint != op.request.endpoint || cluster != req_cluster {
            return false;
        }
        match expect {
            Expect::None => false,
            Expect::ZclResponse { command: want } => command == want,
            Expect::DefaultResponse { to_command } => {
                command == 0x0B && rsp_to_cmd == Some(to_command)
            }
        }
    }

    fn apply_response(&mut self, index: usize, response: Event) {
        let status = match response {
            Event::ZclResponse { status, .. } => status.unwrap_or(0),
            _ => return,
        };
        self.finish(
            index,
            if status == 0 {
                Status::Confirmed
            } else {
                Status::Failed
            },
        );
    }

    fn buffer_early(&mut self, response: Event, now: u64) {
        let entry = Early {
            response,
            at: now,
        };
        if let Some(slot) = self.early.iter_mut().find(|slot| slot.is_none()) {
            *slot = Some(entry);
            return;
        }
        // Bounded: drop the oldest entry, never grow.
        if let Some((index, _)) = self
            .early
            .iter()
            .enumerate()
            .filter_map(|(index, slot)| slot.map(|early| (index, early.at)))
            .min_by_key(|(_, at)| *at)
        {
            self.early[index] = Some(entry);
            self.early_dropped = self.early_dropped.saturating_add(1);
        }
    }

    fn take_early_for(&mut self, index: usize, tsn: u8, now: u64) -> Option<Event> {
        let op = self.ops[index].as_ref()?.request;
        let found = self.early.iter().position(|slot| {
            let Some(early) = slot else {
                return false;
            };
            if now.saturating_sub(early.at) > EARLY_TTL_MS {
                return false;
            }
            let mut probe = Op {
                id: 0,
                seq: 0,
                request: op,
                status: Status::Acknowledged,
                attempts: 0,
                tsn: Some(tsn),
                sent_at: now,
                deadline: 0,
                not_before: 0,
                parked: false,
            };
            probe.request.kind = op.kind;
            self.response_matches(&probe, &early.response)
        })?;
        self.early[found].take().map(|early| early.response)
    }

    fn prune_early(&mut self, now: u64) {
        for slot in &mut self.early {
            if let Some(early) = slot {
                if now.saturating_sub(early.at) > EARLY_TTL_MS {
                    *slot = None;
                }
            }
        }
    }

    fn expire(&mut self, now: u64) {
        let expired: [bool; MAX_OPS] =
            std::array::from_fn(|index| match &self.ops[index] {
                Some(op) => !op.status.is_final() && now >= op.deadline,
                None => false,
            });
        for (index, expired) in expired.iter().enumerate() {
            if *expired {
                self.finish(index, Status::Expired);
            }
        }
    }

    fn handle_timeouts(&mut self, now: u64) {
        for index in 0..MAX_OPS {
            let Some(op) = &self.ops[index] else {
                continue;
            };
            if op.parked || !matches!(op.status, Status::Sent | Status::Acknowledged) {
                continue;
            }
            if now < op.sent_at + u64::from(op.request.timeout_ms) {
                continue;
            }
            let max_attempts = op.request.retry.max_attempts.clamp(1, MAX_ATTEMPTS);
            if op.request.sleepy && op.attempts < max_attempts {
                let op = self.ops[index].as_mut().unwrap();
                op.parked = true;
                op.tsn = None;
                continue;
            }
            self.retry_or_finish(index, Status::Timeout, now);
        }
    }

    fn retry_or_finish(&mut self, index: usize, failure: Status, now: u64) {
        let op = self.ops[index].as_ref().unwrap();
        let max_attempts = op.request.retry.max_attempts.clamp(1, MAX_ATTEMPTS);
        if op.attempts < max_attempts {
            let op = self.ops[index].as_mut().unwrap();
            op.status = Status::Queued;
            op.tsn = None;
            op.parked = false;
            op.not_before = now + RETRY_BACKOFF_MS;
            return;
        }
        self.finish(index, failure);
    }

    fn send_queued<T: Transport>(&mut self, transport: &mut T, now: u64) {
        loop {
            let index = self.next_sendable(now);
            let Some(index) = index else {
                return;
            };
            let (id, request) = {
                let op = self.ops[index].as_ref().unwrap();
                (op.id, op.request)
            };
            match transport.send(id, &request) {
                Ok(()) => {
                    let op = self.ops[index].as_mut().unwrap();
                    op.status = Status::Sent;
                    op.attempts = op.attempts.saturating_add(1);
                    op.tsn = None;
                    op.sent_at = now;
                    op.parked = false;
                }
                Err(SendError::Full) => return,
                Err(_) => self.finish(index, Status::Failed),
            }
        }
    }

    fn next_sendable(&self, now: u64) -> Option<usize> {
        let mut best: Option<(u32, usize)> = None;
        for (index, op) in self.ops.iter().enumerate() {
            let Some(op) = op else {
                continue;
            };
            if op.status != Status::Queued || now >= op.deadline || now < op.not_before {
                continue;
            }
            let busy = self.ops.iter().flatten().any(|other| {
                other.id != op.id
                    && other.request.device == op.request.device
                    && other.request.endpoint == op.request.endpoint
                    && matches!(other.status, Status::Sent | Status::Acknowledged)
            });
            if busy {
                continue;
            }
            if best.is_none_or(|(seq, _)| op.seq < seq) {
                best = Some((op.seq, index));
            }
        }
        best.map(|(_, index)| index)
    }

    fn find_index(&self, id: OperationId) -> Option<usize> {
        self.ops
            .iter()
            .position(|op| op.as_ref().is_some_and(|op| op.id == id))
    }

    fn finish(&mut self, index: usize, status: Status) {
        let Some(op) = self.ops[index].take() else {
            return;
        };
        if self.results.len() == MAX_RESULTS {
            self.results.pop_front();
            self.results_dropped = self.results_dropped.saturating_add(1);
        }
        self.results.push_back(OperationResult { id: op.id, status });
    }
}

pub mod fake {
    use super::*;

    #[derive(Clone, Debug)]
    pub struct Sent {
        pub op: OperationId,
        pub request: Request,
    }

    #[derive(Default)]
    pub struct FakeTransport {
        pub sent: Vec<Sent>,
        events: VecDeque<Event>,
        pub fail_next: bool,
    }

    impl FakeTransport {
        pub fn new() -> Self {
            Self::default()
        }

        pub fn confirm(&mut self, op: OperationId, tsn: u8, success: bool) {
            self.events.push_back(Event::Confirm { op, tsn, success });
        }

        #[allow(clippy::too_many_arguments)]
        pub fn response(
            &mut self,
            src: u16,
            endpoint: u8,
            cluster: u16,
            command: u8,
            tsn: u8,
            rsp_to_cmd: Option<u8>,
            status: Option<u8>,
        ) {
            self.events.push_back(Event::ZclResponse {
                src,
                endpoint,
                cluster,
                command,
                tsn,
                rsp_to_cmd,
                status,
            });
        }

        pub fn zdo(&mut self, op: OperationId, success: bool) {
            self.events.push_back(Event::ZdoResult { op, success });
        }

        pub fn last(&self) -> Option<&Sent> {
            self.sent.last()
        }
    }

    impl Transport for FakeTransport {
        fn send(&mut self, op: OperationId, request: &Request) -> Result<(), SendError> {
            if self.fail_next {
                self.fail_next = false;
                return Err(SendError::Io);
            }
            self.sent.push(Sent {
                op,
                request: *request,
            });
            Ok(())
        }

        fn poll(&mut self) -> Option<Event> {
            self.events.pop_front()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::fake::FakeTransport;
    use super::*;

    const SHORT: u16 = 0x1234;
    const EP: u8 = 1;

    fn zcl(command: u8, expect: Expect, idempotent: bool) -> Request {
        Request {
            device: 0xA4C1_383B_F9D6_2A03,
            short: SHORT,
            endpoint: EP,
            sleepy: false,
            retry: if idempotent { RetryPolicy::SAFE } else { RetryPolicy::NEVER },
            expiry_ms: 10_000,
            timeout_ms: 1_000,
            kind: RequestKind::Zcl {
                cluster: 0x0006,
                command,
                payload: Payload::new(&[]).unwrap(),
                expect,
            },
        }
    }

    fn run(exec: &mut Executor, transport: &mut FakeTransport, now: u64) {
        exec.tick(transport, now);
    }

    #[test]
    fn af_confirm_only_finishes_acknowledged() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x01, Expect::None, true), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        assert_eq!(exec.status(id), Some(Status::Sent));
        transport.confirm(id, 7, true);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Acknowledged);
        assert_eq!(exec.status(id), None);
    }

    #[test]
    fn zcl_response_confirms_after_confirm() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 9, true);
        run(&mut exec, &mut transport, 10);
        transport.response(SHORT, EP, 0x0006, 0x01, 9, None, None);
        run(&mut exec, &mut transport, 20);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn response_before_confirm_is_buffered() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        transport.response(SHORT, EP, 0x0006, 0x01, 42, None, None);
        run(&mut exec, &mut transport, 5);
        transport.confirm(id, 42, true);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn wrong_context_and_tsn_are_ignored() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 9, true);
        run(&mut exec, &mut transport, 10);
        transport.response(SHORT, EP, 0x0006, 0x01, 8, None, None); // wrong tsn
        transport.response(SHORT, EP, 0x0001, 0x01, 9, None, None); // wrong cluster
        transport.response(0x9999, EP, 0x0006, 0x01, 9, None, None); // wrong address
        run(&mut exec, &mut transport, 20);
        assert_eq!(exec.status(id), Some(Status::Acknowledged));
        transport.response(SHORT, EP, 0x0006, 0x01, 9, None, None);
        run(&mut exec, &mut transport, 30);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn idempotent_retry_after_timeout() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        run(&mut exec, &mut transport, 1_001);
        assert_eq!(exec.status(id), Some(Status::Queued));
        run(&mut exec, &mut transport, 1_201);
        assert_eq!(transport.sent.len(), 2);
        transport.confirm(id, 3, true);
        run(&mut exec, &mut transport, 1_010);
        assert_eq!(exec.take_result().unwrap().status, Status::Acknowledged);
    }

    #[test]
    fn non_idempotent_command_is_not_retried() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x02, Expect::None, false), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        run(&mut exec, &mut transport, 1_001);
        assert_eq!(exec.take_result().unwrap().status, Status::Timeout);
        assert_eq!(transport.sent.len(), 1);
    }

    #[test]
    fn queued_request_expires_before_send() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let first = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        let mut second = zcl(0x01, Expect::None, true);
        second.expiry_ms = 500;
        let second = exec.enqueue(second, 0).unwrap();
        run(&mut exec, &mut transport, 600);
        assert_eq!(exec.take_result().unwrap().status, Status::Expired);
        assert_eq!(exec.status(second), None);
        transport.confirm(first, 1, true);
        run(&mut exec, &mut transport, 610);
        assert_eq!(transport.sent.len(), 1);
    }

    #[test]
    fn per_endpoint_serialization_but_parallel_across_endpoints() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let a = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        let b = exec.enqueue(zcl(0x01, Expect::None, true), 0).unwrap();
        let mut other = zcl(0x00, Expect::None, true);
        other.endpoint = 2;
        other.device = 0xB;
        let c = exec.enqueue(other, 0).unwrap();
        run(&mut exec, &mut transport, 0);
        assert_eq!(transport.sent.len(), 2);
        let sent_ids: Vec<_> = transport.sent.iter().map(|sent| sent.op).collect();
        assert!(sent_ids.contains(&a));
        assert!(sent_ids.contains(&c));
        assert!(!sent_ids.contains(&b));
        transport.confirm(a, 1, true);
        run(&mut exec, &mut transport, 10);
        run(&mut exec, &mut transport, 10);
        assert!(transport.sent.iter().any(|sent| sent.op == b));
    }

    #[test]
    fn zdo_bind_finishes_on_callback_without_tsn() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let request = Request {
            device: 1,
            short: SHORT,
            endpoint: EP,
            sleepy: false,
            retry: RetryPolicy::SAFE,
            expiry_ms: 10_000,
            timeout_ms: 1_000,
            kind: RequestKind::ZdoBind { cluster: 0x0006 },
        };
        let id = exec.enqueue(request, 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.zdo(id, true);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn sleepy_parks_without_blocking_other_queues_then_resumes() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let mut sleepy = zcl(0x00, Expect::None, true);
        sleepy.sleepy = true;
        let id = exec.enqueue(sleepy, 0).unwrap();
        let mut other = zcl(0x00, Expect::None, true);
        other.device = 2;
        other.endpoint = 3;
        let other_id = exec.enqueue(other, 0).unwrap();
        run(&mut exec, &mut transport, 0);
        run(&mut exec, &mut transport, 1_001);
        assert_eq!(exec.status(id), Some(Status::Sent));
        assert!(transport.sent.iter().any(|sent| sent.op == other_id));
        exec.device_awake(SHORT);
        run(&mut exec, &mut transport, 1_100);
        assert_eq!(transport.sent.iter().filter(|sent| sent.op == id).count(), 2);
        transport.confirm(id, 4, true);
        run(&mut exec, &mut transport, 1_110);
        assert_eq!(exec.take_result().unwrap().status, Status::Acknowledged);
    }

    #[test]
    fn queue_full_and_payload_limits_are_explicit() {
        let mut exec = Executor::new();
        for _ in 0..MAX_OPS {
            exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        }
        assert_eq!(
            exec.enqueue(zcl(0x00, Expect::None, true), 0),
            Err(ExecError::QueueFull)
        );
        assert_eq!(
            Payload::new(&[0; MAX_PAYLOAD + 1]),
            Err(ExecError::PayloadTooLarge)
        );
    }

    #[test]
    fn early_response_buffer_is_bounded() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        for tsn in 0..(MAX_EARLY as u8 + 3) {
            transport.response(SHORT, EP, 0x0006, 0x01, tsn + 1, None, None);
            run(&mut exec, &mut transport, u64::from(tsn));
        }
        assert_eq!(exec.early_dropped(), 3);
        assert_eq!(exec.status(id), Some(Status::Sent));
    }

    #[test]
    fn confirm_failure_retries_idempotent_and_fails_non_idempotent() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 1, false);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.status(id), Some(Status::Queued));
        run(&mut exec, &mut transport, 210);
        assert_eq!(exec.status(id), Some(Status::Sent));
        assert_eq!(transport.sent.len(), 2);

        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x02, Expect::None, false), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 1, false);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Failed);
    }
}
