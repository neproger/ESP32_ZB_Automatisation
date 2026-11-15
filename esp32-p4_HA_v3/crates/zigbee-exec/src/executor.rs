use std::collections::VecDeque;
use crate::types::*;

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
    /// Recent completed request contexts, keyed by OperationId. Bounded like
    /// results; `take_context` lets consumers decode Read payloads without
    /// duplicating request data in every result.
    contexts: VecDeque<(OperationId, Request)>,
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
            contexts: VecDeque::with_capacity(MAX_RESULTS),
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

    /// Context of a recently completed operation (bounded). Used to decode
    /// Read Attributes payloads through the normal profile decoder.
    pub fn take_context(&mut self, id: OperationId) -> Option<Request> {
        let index = self
            .contexts
            .iter()
            .position(|(context_id, _)| *context_id == id)?;
        self.contexts.remove(index).map(|(_, request)| request)
    }
    pub fn pending_leave(&self, device: u64) -> bool {
        self.ops.iter().flatten().any(|op| {
            op.request.device == device && matches!(op.request.kind, RequestKind::ZdoLeave)
        })
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
                    read,
                } => self.on_response(
                    src, endpoint, cluster, command, tsn, rsp_to_cmd, status, read, now,
                ),
                Event::ZdoResult { op, success } => self.on_zdo_result(op, success, now),
                Event::ZdoActiveEndpoints { op, endpoints } => {
                    self.on_zdo_data(op, ZdoData::ActiveEndpoints(endpoints))
                }
                Event::ZdoSimpleDescriptor { op, descriptor } => {
                    self.on_zdo_data(op, ZdoData::SimpleDescriptor(descriptor))
                }
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
            Some(RequestKind::SendZcl { expect, .. }) => expect,
            Some(RequestKind::ReadAttributes { .. }) => Expect::ZclResponse { command: 0x01 },
            Some(RequestKind::ConfigureReporting { .. }) => Expect::ZclResponse { command: 0x07 },
            Some(RequestKind::ZdoBind { .. })
            | Some(RequestKind::ZdoLeave)
            | Some(RequestKind::ZdoActiveEndpoints)
            | Some(RequestKind::ZdoSimpleDescriptor { .. }) => {
                let op = self.ops[index].as_mut().unwrap();
                op.status = Status::Acknowledged;
                op.sent_at = now;
                return;
            }
            None => return,
        };

        if expect == Expect::None {
            self.finish(index, Status::Acknowledged, None);
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
        read: Option<Payload>,
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
            read,
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

    fn on_zdo_data(&mut self, id: OperationId, data: ZdoData) {
        let Some(index) = self.find_index(id) else {
            return;
        };
        let matches = matches!(
            (self.ops[index].as_ref().map(|op| op.request.kind), data),
            (
                Some(RequestKind::ZdoActiveEndpoints),
                ZdoData::ActiveEndpoints(_)
            ) | (
                Some(RequestKind::ZdoSimpleDescriptor { .. }),
                ZdoData::SimpleDescriptor(_)
            )
        );
        if !matches {
            return;
        }
        self.finish(index, Status::Confirmed, None);
        if let Some(result) = self.results.back_mut() {
            result.zdo = Some(data);
        }
    }

    fn on_zdo_result(&mut self, id: OperationId, success: bool, now: u64) {
        let Some(index) = self.find_index(id) else {
            return;
        };
        let is_zdo = matches!(
            self.ops[index].as_ref().map(|op| op.request.kind),
            Some(RequestKind::ZdoBind { .. }) | Some(RequestKind::ZdoLeave)
        );
        if !is_zdo {
            return;
        }
        let _ = now;
        if success {
            self.finish(index, Status::Confirmed, None);
        } else {
            self.retry_or_finish(index, Status::Failed, now);
        }
    }

    fn response_matches(&self, op: &Op, response: &Event) -> bool {
        let (req_cluster, expect) = match op.request.kind {
            RequestKind::Zcl {
                cluster, expect, ..
            }
            | RequestKind::SendZcl {
                cluster, expect, ..
            } => (cluster, expect),
            RequestKind::ReadAttributes { cluster, .. } => {
                (cluster, Expect::ZclResponse { command: 0x01 })
            }
            RequestKind::ConfigureReporting { cluster, .. } => {
                (cluster, Expect::ZclResponse { command: 0x07 })
            }
            RequestKind::ZdoBind { .. }
            | RequestKind::ZdoActiveEndpoints
            | RequestKind::ZdoSimpleDescriptor { .. }
            | RequestKind::ZdoLeave => return false,
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
        let (status, read) = match response {
            Event::ZclResponse { status, read, .. } => (status.unwrap_or(0), read),
            _ => return,
        };
        if status == 0 {
            self.finish(index, Status::Confirmed, read);
        } else {
            self.finish(index, Status::Failed, None);
        }
    }

    fn buffer_early(&mut self, response: Event, now: u64) {
        let entry = Early { response, at: now };
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
        let expired: [bool; MAX_OPS] = std::array::from_fn(|index| match &self.ops[index] {
            Some(op) => !op.status.is_final() && now >= op.deadline,
            None => false,
        });
        for (index, expired) in expired.iter().enumerate() {
            if *expired {
                self.finish(index, Status::Expired, None);
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
        self.finish(index, failure, None);
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
                Err(_) => self.finish(index, Status::Failed, None),
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

    fn finish(&mut self, index: usize, status: Status, read: Option<Payload>) {
        let Some(op) = self.ops[index].take() else {
            return;
        };
        // Central guard: only a confirmed operation may expose read data.
        let read = read.filter(|_| status == Status::Confirmed);
        if self.results.len() == MAX_RESULTS {
            self.results.pop_front();
            self.results_dropped = self.results_dropped.saturating_add(1);
        }
        self.results.push_back(OperationResult {
            id: op.id,
            status,
            read,
            zdo: None,
        });
        if self.contexts.len() == MAX_RESULTS {
            self.contexts.pop_front();
        }
        self.contexts.push_back((op.id, op.request));
    }
}

