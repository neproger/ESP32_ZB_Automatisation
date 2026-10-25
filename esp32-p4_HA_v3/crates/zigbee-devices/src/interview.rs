//! Interview state machine: one independent job per EUI64.
//!
//! `zigbee-exec` owns TSN/waiter correlation and transport retries; this module
//! only tracks what is still unknown about a device and asks the next question.
//! A result is applied only if its `OperationId` is the job's current pending
//! operation, so a late result from an older job cannot change a newer one.

use crate::descriptor::{ClusterRole, DeviceDescriptor, EndpointDescriptor};
use z2m_rs::{EndpointIdentity, Interview, Profile};
use zigbee_exec::{
    AttributeList, Executor, OperationId, OperationResult, Request, RequestKind, RetryPolicy,
    Status, ZdoData,
};

pub const JOB_SLOTS: usize = 8;
pub const MAX_ATTEMPTS: u8 = 3;
pub const RETRY_BACKOFF_MS: u64 = 5_000;
/// Longer than the executor's own expiry: its final result is consumed first.
pub const OP_DEADLINE_MS: u64 = 30_000;
pub const COOLDOWN_MS: u64 = 30_000;

const OP_EXPIRY_MS: u32 = 20_000;
const READ_TIMEOUT_MS: u32 = 3_000;
const BASIC_CLUSTER: u16 = 0x0000;
const ENDPOINT: u8 = 1;
const ATTR_MANUFACTURER: u16 = 0x0004;
const ATTR_MODEL: u16 = 0x0005;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Step {
    Manufacturer,
    Model,
    ActiveEndpoints,
    SimpleDescriptor,
}

impl Step {
    fn attr(self) -> u16 {
        match self {
            Self::Manufacturer => ATTR_MANUFACTURER,
            Self::Model => ATTR_MODEL,
            Self::ActiveEndpoints | Self::SimpleDescriptor => 0,
        }
    }
}

#[derive(Clone, Copy)]
struct Job {
    eui: u64,
    short: u16,
    manufacturer: [u8; 32],
    man_len: u8,
    model: [u8; 32],
    model_len: u8,
    step: Step,
    profile: Option<&'static Profile>,
    descriptor: DeviceDescriptor,
    active: [u8; 8],
    active_count: u8,
    active_index: u8,
    pending: Option<OperationId>,
    started_at: u64,
    attempts: u8,
    retry_at: u64,
}

impl Job {
    fn new(eui: u64, short: u16) -> Self {
        Self {
            eui,
            short,
            manufacturer: [0; 32],
            man_len: 0,
            model: [0; 32],
            model_len: 0,
            step: Step::Manufacturer,
            profile: None,
            descriptor: DeviceDescriptor::new(),
            active: [0; 8],
            active_count: 0,
            active_index: 0,
            pending: None,
            started_at: 0,
            attempts: 0,
            retry_at: 0,
        }
    }
}

#[derive(Clone)]
pub struct Identified {
    pub eui: u64,
    pub short: u16,
    pub manufacturer: String,
    pub model: String,
    pub profile: &'static Profile,
    pub descriptor: DeviceDescriptor,
}

pub struct InterviewManager {
    jobs: [Option<Job>; JOB_SLOTS],
    cooldown: [(u64, u64); JOB_SLOTS],
}

impl Default for InterviewManager {
    fn default() -> Self {
        Self::new()
    }
}

impl InterviewManager {
    pub const fn new() -> Self {
        Self {
            jobs: [None; JOB_SLOTS],
            cooldown: [(0, 0); JOB_SLOTS],
        }
    }

    pub fn is_interviewing(&self, eui: u64) -> bool {
        self.jobs.iter().flatten().any(|job| job.eui == eui)
    }

    pub fn job_count(&self) -> usize {
        self.jobs.iter().flatten().count()
    }

    /// Cancels an in-flight interview, e.g. when the HA device is removed.
    pub fn forget(&mut self, eui: u64) {
        for job in &mut self.jobs {
            if job.as_ref().is_some_and(|job| job.eui == eui) {
                *job = None;
            }
        }
    }

    /// Starts an interview unless the device is known, already being
    /// interviewed, cooling down after a failure, or all slots are taken.
    pub fn start(
        &mut self,
        exec: &mut Executor,
        eui: u64,
        short: u16,
        known: bool,
        now: u64,
    ) -> bool {
        if eui == 0 || known || self.is_interviewing(eui) || self.in_cooldown(eui, now) {
            return false;
        }
        let Some(slot) = self.jobs.iter().position(Option::is_none) else {
            return false;
        };
        let mut job = Job::new(eui, short);
        if enqueue(&mut job, exec, now).is_err() {
            job.retry_at = now + RETRY_BACKOFF_MS;
        }
        self.jobs[slot] = Some(job);
        true
    }

    /// Feeds an executor result; returns the device when the interview is complete.
    pub fn on_result(
        &mut self,
        exec: &mut Executor,
        result: OperationResult,
        now: u64,
    ) -> Option<Identified> {
        let slot = self.jobs.iter().position(|job| {
            job.as_ref()
                .is_some_and(|job| job.pending == Some(result.id))
        })?;
        let job = self.jobs[slot].as_mut().unwrap();
        if result.status == Status::Confirmed {
            match job.step {
                Step::Manufacturer | Step::Model => {
                    let attr = job.step.attr();
                    let Some(value) = result
                        .read
                        .as_ref()
                        .and_then(|payload| find_string(payload.as_slice(), attr))
                    else {
                        return self.failed(slot, now);
                    };
                    let len = usize::from(value[0])
                        .min(value.len().saturating_sub(1))
                        .min(31);
                    if job.step == Step::Manufacturer {
                        job.manufacturer[..len].copy_from_slice(&value[1..1 + len]);
                        job.man_len = len as u8;
                        job.step = Step::Model;
                        job.attempts = 0;
                        job.pending = None;
                        if enqueue(job, exec, now).is_err() {
                            job.retry_at = now + RETRY_BACKOFF_MS;
                        }
                        None
                    } else {
                        job.model[..len].copy_from_slice(&value[1..1 + len]);
                        job.model_len = len as u8;
                        job.profile = identify_profile(job);
                        job.step = Step::ActiveEndpoints;
                        job.attempts = 0;
                        job.pending = None;
                        if enqueue(job, exec, now).is_err() {
                            job.retry_at = now + RETRY_BACKOFF_MS;
                        }
                        None
                    }
                }
                Step::ActiveEndpoints => {
                    let Some(ZdoData::ActiveEndpoints(endpoints)) = result.zdo else {
                        return self.failed(slot, now);
                    };
                    if endpoints.len() == 0 || endpoints.len() > job.active.len() {
                        return self.failed(slot, now);
                    }
                    for (index, endpoint) in endpoints.as_slice().iter().enumerate() {
                        job.active[index] = *endpoint;
                    }
                    job.active_count = endpoints.len() as u8;
                    job.active_index = 0;
                    job.step = Step::SimpleDescriptor;
                    job.attempts = 0;
                    job.pending = None;
                    if enqueue(job, exec, now).is_err() {
                        job.retry_at = now + RETRY_BACKOFF_MS;
                    }
                    None
                }
                Step::SimpleDescriptor => {
                    let Some(ZdoData::SimpleDescriptor(descriptor)) = result.zdo else {
                        return self.failed(slot, now);
                    };
                    if !push_simple_descriptor(job, descriptor) {
                        return self.failed(slot, now);
                    }
                    job.active_index += 1;
                    if job.active_index < job.active_count {
                        job.pending = None;
                        job.attempts = 0;
                        if enqueue(job, exec, now).is_err() {
                            job.retry_at = now + RETRY_BACKOFF_MS;
                        }
                        None
                    } else {
                        let identified = identify_finished(job);
                        self.jobs[slot] = None;
                        identified
                    }
                }
            }
        } else if matches!(
            result.status,
            Status::Timeout | Status::Failed | Status::Expired
        ) {
            job.pending = None;
            self.failed(slot, now)
        } else {
            None
        }
    }

    /// Retries missing steps and recovers a pending operation whose result was
    /// never delivered (bounded executor result queue or a lost event).
    pub fn tick(&mut self, exec: &mut Executor, now: u64) {
        let mut expired: [Option<(usize, u64)>; JOB_SLOTS] = [None; JOB_SLOTS];
        for slot in 0..JOB_SLOTS {
            let Some(job) = self.jobs[slot].as_mut() else {
                continue;
            };
            if job.pending.is_some() && now.saturating_sub(job.started_at) >= OP_DEADLINE_MS {
                job.pending = None;
                job.attempts = job.attempts.saturating_add(1);
                if job.attempts >= MAX_ATTEMPTS {
                    expired[slot] = Some((slot, job.eui));
                    continue;
                }
                job.retry_at = now + RETRY_BACKOFF_MS;
            }
            if job.pending.is_none() && job.retry_at != 0 && now >= job.retry_at {
                job.retry_at = 0;
                if enqueue(job, exec, now).is_err() {
                    job.retry_at = now + RETRY_BACKOFF_MS;
                }
            }
        }
        for (slot, eui) in expired.into_iter().flatten() {
            self.jobs[slot] = None;
            self.set_cooldown(eui, now);
        }
    }

    fn failed(&mut self, slot: usize, now: u64) -> Option<Identified> {
        let job = self.jobs[slot].as_mut().unwrap();
        // A known profile or a partially discovered endpoint still has a working
        // decode path when the device does not answer the rest of ZDO.
        if matches!(job.step, Step::ActiveEndpoints | Step::SimpleDescriptor)
            && (job.profile.is_some() || job.descriptor.endpoints().next().is_some())
        {
            let identified = identify_finished(job);
            self.jobs[slot] = None;
            return identified;
        }
        job.attempts = job.attempts.saturating_add(1);
        if job.attempts >= MAX_ATTEMPTS {
            let eui = job.eui;
            self.jobs[slot] = None;
            self.set_cooldown(eui, now);
        } else {
            job.retry_at = now + RETRY_BACKOFF_MS;
        }
        None
    }

    fn in_cooldown(&self, eui: u64, now: u64) -> bool {
        self.cooldown
            .iter()
            .any(|(cooldown_eui, until)| *cooldown_eui == eui && now < *until)
    }

    fn set_cooldown(&mut self, eui: u64, now: u64) {
        let until = now + COOLDOWN_MS;
        if let Some(entry) = self
            .cooldown
            .iter_mut()
            .find(|(cooldown_eui, until)| *cooldown_eui == eui || now >= *until)
        {
            *entry = (eui, until);
            return;
        }
        if let Some(entry) = self.cooldown.iter_mut().min_by_key(|(_, until)| *until) {
            *entry = (eui, until);
        }
    }
}

fn enqueue(job: &mut Job, exec: &mut Executor, now: u64) -> Result<(), ()> {
    let kind = match job.step {
        Step::Manufacturer | Step::Model => RequestKind::ReadAttributes {
            cluster: BASIC_CLUSTER,
            attributes: AttributeList::single(job.step.attr()),
        },
        Step::ActiveEndpoints => RequestKind::ZdoActiveEndpoints,
        Step::SimpleDescriptor => RequestKind::ZdoSimpleDescriptor {
            endpoint: job.active[usize::from(job.active_index)],
        },
    };
    let request = Request {
        device: job.eui,
        short: job.short,
        endpoint: if matches!(job.step, Step::Manufacturer | Step::Model) {
            ENDPOINT
        } else {
            0
        },
        sleepy: true,
        retry: RetryPolicy::SAFE,
        expiry_ms: OP_EXPIRY_MS,
        timeout_ms: READ_TIMEOUT_MS,
        kind,
    };
    match exec.enqueue(request, now) {
        Ok(id) => {
            job.pending = Some(id);
            job.started_at = now;
            Ok(())
        }
        Err(_) => {
            job.pending = None;
            Err(())
        }
    }
}

fn push_simple_descriptor(job: &mut Job, descriptor: zigbee_exec::SimpleDescriptor) -> bool {
    let mut endpoint = EndpointDescriptor::new(
        descriptor.endpoint,
        descriptor.profile_id,
        descriptor.device_id,
    );
    for cluster in descriptor.server_clusters.as_slice() {
        if !endpoint.push(*cluster, ClusterRole::Server) {
            return false;
        }
    }
    for cluster in descriptor.client_clusters.as_slice() {
        if !endpoint.push(*cluster, ClusterRole::Client) {
            return false;
        }
    }
    job.descriptor.push(endpoint)
}
fn identify_finished(job: &Job) -> Option<Identified> {
    let manufacturer =
        String::from_utf8_lossy(&job.manufacturer[..usize::from(job.man_len)]).to_string();
    let model = String::from_utf8_lossy(&job.model[..usize::from(job.model_len)]).to_string();
    let profile = identify_with_descriptor(job, &manufacturer, &model)
        .or(job.profile)
        .unwrap_or(&crate::standard::STANDARD_PROFILE);
    Some(Identified {
        eui: job.eui,
        short: job.short,
        manufacturer,
        model,
        profile,
        descriptor: job.descriptor,
    })
}

struct OwnedEndpoint {
    id: u8,
    device_id: u16,
    profile_id: u16,
    input: Vec<u16>,
    output: Vec<u16>,
}

/// Fingerprints may differ only by endpoint/cluster sets; pass the discovered
/// descriptor so the matcher can tell those definitions apart.
fn identify_with_descriptor(
    job: &Job,
    manufacturer: &str,
    model: &str,
) -> Option<&'static Profile> {
    let owned: Vec<OwnedEndpoint> = job
        .descriptor
        .endpoints()
        .map(|endpoint| OwnedEndpoint {
            id: endpoint.endpoint,
            device_id: endpoint.device_id,
            profile_id: endpoint.profile_id,
            input: endpoint
                .clusters()
                .iter()
                .filter(|entry| entry.role == ClusterRole::Server)
                .map(|entry| entry.cluster_id)
                .collect(),
            output: endpoint
                .clusters()
                .iter()
                .filter(|entry| entry.role == ClusterRole::Client)
                .map(|entry| entry.cluster_id)
                .collect(),
        })
        .collect();
    if owned.is_empty() {
        return None;
    }
    let endpoints: Vec<EndpointIdentity<'_>> = owned
        .iter()
        .map(|endpoint| EndpointIdentity {
            id: endpoint.id,
            device_id: Some(endpoint.device_id),
            profile_id: Some(endpoint.profile_id),
            input: &endpoint.input,
            output: &endpoint.output,
        })
        .collect();
    z2m_rs::identify_with_endpoints(
        &Interview {
            manufacturer: Some(manufacturer),
            model,
        },
        &endpoints,
    )
}

fn identify_profile(job: &Job) -> Option<&'static Profile> {
    let manufacturer =
        String::from_utf8_lossy(&job.manufacturer[..usize::from(job.man_len)]).to_string();
    let model = String::from_utf8_lossy(&job.model[..usize::from(job.model_len)]).to_string();
    z2m_rs::identify(&Interview {
        manufacturer: Some(&manufacturer),
        model: &model,
    })
}

/// First successful string record for `wanted`; records are
/// `attr_id u16 | type | value`, the format the C bridge emits.
fn find_string(bytes: &[u8], wanted: u16) -> Option<&[u8]> {
    let mut index = 0;
    while index + 3 <= bytes.len() {
        let attr = u16::from_le_bytes([bytes[index], bytes[index + 1]]);
        let ty = bytes[index + 2];
        let value = &bytes[index + 3..];
        if ty != 0x41 && ty != 0x42 {
            return None;
        }
        let len = usize::from(*value.first()?) + 1;
        if len > value.len() {
            return None;
        }
        if attr == wanted {
            return Some(&value[..len]);
        }
        index += 3 + len;
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use zigbee_exec::fake::{FakeTransport, Sent};
    use zigbee_exec::{ClusterList, EndpointList, Event, Payload, SimpleDescriptor};

    const A: u64 = 0x0012_4b00_0a1b_2c3d;
    const B: u64 = 0x0012_4b00_0d1e_2f40;

    fn payload(attribute: u16, text: &str) -> Payload {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&attribute.to_le_bytes());
        bytes.push(0x42);
        bytes.push(text.len() as u8);
        bytes.extend_from_slice(text.as_bytes());
        Payload::new(&bytes).unwrap()
    }

    fn sent_for(transport: &FakeTransport, eui: u64, attribute: u16) -> Sent {
        transport
            .sent
            .iter()
            .rev()
            .find(|sent| {
                sent.request.device == eui
                    && matches!(
                        sent.request.kind,
                        RequestKind::ReadAttributes { attributes, .. }
                            if attributes.as_slice() == [attribute]
                    )
            })
            .cloned()
            .expect("request was not sent")
    }

    fn serve(
        exec: &mut Executor,
        transport: &mut FakeTransport,
        manager: &mut InterviewManager,
        now: u64,
        eui: u64,
        attribute: u16,
        text: &str,
    ) -> Option<Identified> {
        let sent = sent_for(transport, eui, attribute);
        let tsn = (sent.op % 30) as u8 + 1;
        transport.confirm(sent.op, tsn, true);
        exec.tick(transport, now);
        transport
            .response_read(
                sent.request.short,
                ENDPOINT,
                BASIC_CLUSTER,
                0x01,
                tsn,
                None,
                payload(attribute, text).as_slice(),
            )
            .unwrap();
        exec.tick(transport, now + 1);
        let mut identified = None;
        while let Some(result) = exec.take_result() {
            if let Some(found) = manager.on_result(exec, result, now + 1) {
                identified = Some(found);
            }
        }
        if attribute == ATTR_MODEL && identified.is_none() {
            identified = finish_discovery(exec, transport, manager, eui, now + 2);
        }
        identified
    }

    fn finish_discovery(
        exec: &mut Executor,
        transport: &mut FakeTransport,
        manager: &mut InterviewManager,
        _eui: u64,
        now: u64,
    ) -> Option<Identified> {
        exec.tick(transport, now);
        let active = transport.last().expect("active endpoints").op;
        let mut endpoints = EndpointList::new();
        assert!(endpoints.push(1));
        transport.push_event(Event::ZdoActiveEndpoints {
            op: active,
            endpoints,
        });
        exec.tick(transport, now + 1);
        while let Some(result) = exec.take_result() {
            assert!(manager.on_result(exec, result, now + 1).is_none());
        }
        exec.tick(transport, now + 2);
        let simple = transport.last().expect("simple descriptor").op;
        let mut server = ClusterList::new();
        assert!(server.push(0x0006));
        transport.push_event(Event::ZdoSimpleDescriptor {
            op: simple,
            descriptor: SimpleDescriptor {
                endpoint: 1,
                profile_id: 0x0104,
                device_id: 0x0000,
                server_clusters: server,
                client_clusters: ClusterList::new(),
            },
        });
        exec.tick(transport, now + 3);
        exec.take_result()
            .and_then(|result| manager.on_result(exec, result, now + 3))
    }

    #[test]
    fn two_devices_interview_independently() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        assert!(manager.start(&mut exec, A, 0x1001, false, 0));
        assert!(manager.start(&mut exec, B, 0x1002, false, 0));
        exec.tick(&mut transport, 0);
        assert_eq!(manager.job_count(), 2);

        assert!(serve(
            &mut exec,
            &mut transport,
            &mut manager,
            2,
            A,
            ATTR_MANUFACTURER,
            "_TZ3000_b4awzgct"
        )
        .is_none());
        exec.tick(&mut transport, 4);
        assert!(serve(
            &mut exec,
            &mut transport,
            &mut manager,
            6,
            B,
            ATTR_MANUFACTURER,
            "eWeLink"
        )
        .is_none());
        exec.tick(&mut transport, 8);

        let a = serve(
            &mut exec,
            &mut transport,
            &mut manager,
            10,
            A,
            ATTR_MODEL,
            "TS0041",
        )
        .expect("A identified");
        let b = serve(
            &mut exec,
            &mut transport,
            &mut manager,
            12,
            B,
            ATTR_MODEL,
            "SNZB-01P",
        )
        .expect("B identified");
        assert_eq!(a.profile.id, "tuya.ts0041");
        assert_eq!(b.profile.id, "sonoff.snzb01p");
        assert_eq!(manager.job_count(), 0);
    }

    #[test]
    fn duplicate_activity_is_ignored() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        assert!(manager.start(&mut exec, A, 0x1001, false, 0));
        assert!(!manager.start(&mut exec, A, 0x1001, false, 0));
        assert!(!manager.start(&mut exec, A, 0x1001, true, 0));
        exec.tick(&mut transport, 0);
        assert_eq!(transport.sent.len(), 1);
        assert_eq!(manager.job_count(), 1);
    }
    #[test]
    fn pending_sleepy_does_not_block_another_device() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        assert!(manager.start(&mut exec, A, 0x1001, false, 0));
        assert!(manager.start(&mut exec, B, 0x1002, false, 0));
        exec.tick(&mut transport, 0);

        assert!(serve(
            &mut exec,
            &mut transport,
            &mut manager,
            2,
            B,
            ATTR_MANUFACTURER,
            "eWeLink"
        )
        .is_none());
        exec.tick(&mut transport, 4);
        let b = serve(
            &mut exec,
            &mut transport,
            &mut manager,
            6,
            B,
            ATTR_MODEL,
            "SNZB-01P",
        )
        .expect("B identified");
        assert_eq!(b.profile.id, "sonoff.snzb01p");
        assert!(manager.is_interviewing(A));
        assert!(!manager.start(&mut exec, A, 0x1001, false, 8));
        assert_eq!(
            transport
                .sent
                .iter()
                .filter(|sent| sent.request.device == A)
                .count(),
            1
        );
    }

    fn fail_three_times(
        exec: &mut Executor,
        transport: &mut FakeTransport,
        manager: &mut InterviewManager,
        eui: u64,
    ) -> u64 {
        let mut now = 0;
        let mut final_deadline = 0;
        for round in 0..3 {
            exec.tick(transport, now);
            let deadline = now + u64::from(OP_EXPIRY_MS);
            exec.tick(transport, deadline);
            while let Some(result) = exec.take_result() {
                manager.on_result(exec, result, deadline);
            }
            final_deadline = deadline;
            now = deadline + RETRY_BACKOFF_MS;
            if round < 2 {
                manager.tick(exec, now);
            }
        }
        assert!(!manager.is_interviewing(eui));
        final_deadline
    }

    #[test]
    fn final_failure_frees_slot_and_blocks_restart_during_cooldown() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        assert!(manager.start(&mut exec, A, 0x1001, false, 0));
        let failed_at = fail_three_times(&mut exec, &mut transport, &mut manager, A);
        assert_eq!(manager.job_count(), 0);
        assert!(!manager.start(&mut exec, A, 0x1001, false, failed_at));
        assert!(manager.start(&mut exec, A, 0x1001, false, failed_at + COOLDOWN_MS));
    }

    #[test]
    fn one_failure_does_not_block_other_devices() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        assert!(manager.start(&mut exec, A, 0x1001, false, 0));
        assert!(manager.start(&mut exec, B, 0x1002, false, 0));
        exec.tick(&mut transport, 0);

        // B progresses to the model read, then A times out while B still waits.
        assert!(serve(
            &mut exec,
            &mut transport,
            &mut manager,
            2,
            B,
            ATTR_MANUFACTURER,
            "eWeLink"
        )
        .is_none());
        exec.tick(&mut transport, 4);

        exec.tick(&mut transport, u64::from(OP_EXPIRY_MS));
        while let Some(result) = exec.take_result() {
            manager.on_result(&mut exec, result, u64::from(OP_EXPIRY_MS));
        }
        assert!(manager.is_interviewing(A));
        assert!(manager.is_interviewing(B));

        let b = serve(
            &mut exec,
            &mut transport,
            &mut manager,
            u64::from(OP_EXPIRY_MS) + 1,
            B,
            ATTR_MODEL,
            "SNZB-01P",
        )
        .expect("B identified despite A failing");
        assert_eq!(b.profile.id, "sonoff.snzb01p");

        manager.tick(&mut exec, u64::from(OP_EXPIRY_MS) + RETRY_BACKOFF_MS);
        exec.tick(&mut transport, u64::from(OP_EXPIRY_MS) + RETRY_BACKOFF_MS);
        assert!(manager.is_interviewing(A));
    }
    #[test]
    fn job_slots_are_bounded_and_reused() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        for index in 0..JOB_SLOTS {
            let eui = 0x0012_4b00_0000_1000 + index as u64;
            assert!(manager.start(&mut exec, eui, 0x1100 + index as u16, false, 0));
        }
        assert!(!manager.start(&mut exec, 0x0012_4b00_0000_2000, 0x1200, false, 0));
        exec.tick(&mut transport, 0);

        assert!(serve(
            &mut exec,
            &mut transport,
            &mut manager,
            2,
            0x0012_4b00_0000_1000,
            ATTR_MANUFACTURER,
            "_TZ3000_b4awzgct"
        )
        .is_none());
        exec.tick(&mut transport, 4);
        serve(
            &mut exec,
            &mut transport,
            &mut manager,
            6,
            0x0012_4b00_0000_1000,
            ATTR_MODEL,
            "TS0041",
        )
        .expect("first device identified");
        assert!(manager.start(&mut exec, 0x0012_4b00_0000_2000, 0x1200, false, 8));
    }

    #[test]
    fn late_result_after_slot_reuse_is_ignored() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        assert!(manager.start(&mut exec, A, 0x1001, false, 0));
        exec.tick(&mut transport, 0);
        let old_op = transport.last().unwrap().op;

        let failed_at = fail_three_times(&mut exec, &mut transport, &mut manager, A);
        let restart = failed_at + COOLDOWN_MS;
        assert!(manager.start(&mut exec, A, 0x1001, false, restart));
        exec.tick(&mut transport, restart);

        let late = OperationResult {
            id: old_op,
            status: Status::Confirmed,
            read: Some(payload(ATTR_MANUFACTURER, "_TZ3000_b4awzgct")),
            zdo: None,
        };
        assert!(manager.on_result(&mut exec, late, restart + 1).is_none());
        assert!(manager.is_interviewing(A));

        assert!(serve(
            &mut exec,
            &mut transport,
            &mut manager,
            restart + 2,
            A,
            ATTR_MANUFACTURER,
            "_TZ3000_b4awzgct"
        )
        .is_none());
        exec.tick(&mut transport, restart + 4);
        let identified = serve(
            &mut exec,
            &mut transport,
            &mut manager,
            restart + 6,
            A,
            ATTR_MODEL,
            "TS0041",
        )
        .expect("new interview completes");
        assert_eq!(identified.profile.id, "tuya.ts0041");
    }

    #[test]
    fn dropped_result_is_recovered_by_deadline() {
        let mut manager = InterviewManager::new();
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        assert!(manager.start(&mut exec, A, 0x1001, false, 0));
        exec.tick(&mut transport, 0);
        assert_eq!(transport.sent.len(), 1);

        manager.tick(&mut exec, OP_DEADLINE_MS);
        assert!(manager.is_interviewing(A));
        manager.tick(&mut exec, OP_DEADLINE_MS + RETRY_BACKOFF_MS);
        exec.tick(&mut transport, OP_DEADLINE_MS + RETRY_BACKOFF_MS);
        assert_eq!(
            transport
                .sent
                .iter()
                .filter(|sent| sent.request.device == A)
                .count(),
            2
        );
        while let Some(result) = exec.take_result() {
            manager.on_result(&mut exec, result, OP_DEADLINE_MS + RETRY_BACKOFF_MS);
        }
        assert!(manager.is_interviewing(A));
    }
}
