//! Device lifecycle: per-EUI interview jobs over `zigbee-exec`.
//!
//! Announce/Authorized/activity queue a device by EUI64. A known device restores
//! its profile from the Registry without a new interview. Unknown devices get a
//! RAM-only job (`ReadManufacturer -> ReadModel -> Identify -> Persist`); jobs are
//! independent, so a sleepy device cannot block others — its reads are parked and
//! resumed by `zigbee-exec` on the next activity. Progress is not persisted: after
//! reboot an unfinished interview simply starts again when the device is seen.

use std::sync::Mutex;

use zigbee_exec::{Executor, Payload, Request, RequestKind, RetryPolicy, Status};

const ATTR_MANUFACTURER: u16 = 0x0004;
const ATTR_MODEL: u16 = 0x0005;
const JOB_SLOTS: usize = 8;
const PROFILE_SLOTS: usize = 8;
const DEVICE_QUEUE: usize = 8;
const MAX_ATTEMPTS: u8 = 3;
const RETRY_BACKOFF_MS: u64 = 5_000;

#[derive(Clone, Copy)]
struct Job {
    ext: u64,
    short: u16,
    manufacturer: [u8; 32],
    man_len: usize,
    model: [u8; 32],
    model_len: usize,
    ops: [Option<u32>; 2],
    attempts: u8,
    retry_at: u64,
}

impl Job {
    const fn new(ext: u64, short: u16) -> Self {
        Self {
            ext,
            short,
            manufacturer: [0; 32],
            man_len: 0,
            model: [0; 32],
            model_len: 0,
            ops: [None; 2],
            attempts: 0,
            retry_at: 0,
        }
    }
}

static JOBS: Mutex<[Option<Job>; JOB_SLOTS]> = Mutex::new([None; JOB_SLOTS]);

#[derive(Clone, Copy)]
struct ProfileMap {
    items: [Option<(u64, &'static z2m_rs::Profile)>; PROFILE_SLOTS],
}

impl ProfileMap {
    const fn new() -> Self {
        Self {
            items: [None; PROFILE_SLOTS],
        }
    }

    fn get(&self, eui: u64) -> Option<&'static z2m_rs::Profile> {
        self.items
            .iter()
            .flatten()
            .find(|(id, _)| *id == eui)
            .map(|(_, profile)| *profile)
    }

    fn insert(&mut self, eui: u64, profile: &'static z2m_rs::Profile) {
        for slot in &mut self.items {
            match slot {
                Some((id, _)) if *id == eui => {
                    *slot = Some((eui, profile));
                    return;
                }
                None => {
                    *slot = Some((eui, profile));
                    return;
                }
                _ => {}
            }
        }
    }
}

static PROFILES: Mutex<ProfileMap> = Mutex::new(ProfileMap::new());

struct DeviceQueue {
    items: [Option<(u64, u16)>; DEVICE_QUEUE],
    head: usize,
    tail: usize,
}

impl DeviceQueue {
    const fn new() -> Self {
        Self {
            items: [None; DEVICE_QUEUE],
            head: 0,
            tail: 0,
        }
    }

    fn push(&mut self, ext: u64, short: u16) {
        if self.items.iter().flatten().any(|queued| queued.0 == ext) {
            return;
        }
        if self.tail - self.head >= DEVICE_QUEUE {
            log::warn!("interview: device queue full, dropping {ext:016x}");
            return;
        }
        self.items[self.tail % DEVICE_QUEUE] = Some((ext, short));
        self.tail += 1;
    }

    fn pop(&mut self) -> Option<(u64, u16)> {
        if self.head == self.tail {
            return None;
        }
        let item = self.items[self.head % DEVICE_QUEUE].take();
        self.head += 1;
        item
    }
}

static DEVICES: Mutex<DeviceQueue> = Mutex::new(DeviceQueue::new());

unsafe extern "C" fn on_device(ext: u64, short: u16) {
    DEVICES.lock().unwrap().push(ext, short);
}

pub fn next_device() -> Option<(u64, u16)> {
    DEVICES.lock().unwrap().pop()
}

extern "C" {
    fn zb_poc_set_device_event_cb(cb: Option<unsafe extern "C" fn(u64, u16)>);
}

pub fn install() {
    unsafe { zb_poc_set_device_event_cb(Some(on_device)) };
    log::info!("device event callback installed");
}

pub fn start(exec: &mut Executor, device: (u64, u16), now: u64) {
    if device.0 == 0 || selected_profile(device.0).is_some() {
        return;
    }
    {
        let mut jobs = JOBS.lock().unwrap();
        if jobs.iter().flatten().any(|job| job.ext == device.0) {
            return;
        }
        let Some(slot) = jobs.iter_mut().find(|slot| slot.is_none()) else {
            log::warn!("interview: job slots full, dropping {:016x}", device.0);
            return;
        };
        *slot = Some(Job::new(device.0, device.1));
    }

    let mut ops = [None; 2];
    for (index, (attr, label)) in
        [(ATTR_MANUFACTURER, "manufacturer"), (ATTR_MODEL, "model")]
            .into_iter()
            .enumerate()
    {
        let request = Request {
            device: device.0,
            short: device.1,
            endpoint: 1,
            // Basic reads of a sleepy device park in zigbee-exec until it wakes.
            sleepy: true,
            retry: RetryPolicy::SAFE,
            expiry_ms: 10_000,
            timeout_ms: 3_000,
            kind: RequestKind::ReadAttributes {
                cluster: 0x0000,
                attribute: attr,
            },
        };
        match exec.enqueue(request, now) {
            Ok(id) => {
                log::info!("interview: read {label} op={id}");
                ops[index] = Some(id);
            }
            Err(error) => log::warn!("interview: {label} not queued: {error:?}"),
        }
    }
    if let Some(job) = JOBS
        .lock()
        .unwrap()
        .iter_mut()
        .flatten()
        .find(|job| job.ext == device.0)
    {
        job.ops = ops;
    }
}

pub fn on_operation_result(id: u32, status: Status, read: Option<Payload>, now: u64) {
    let mut jobs = JOBS.lock().unwrap();
    let Some(index) = jobs
        .iter()
        .position(|slot| slot.as_ref().is_some_and(|job| job.ops.contains(&Some(id))))
    else {
        return;
    };
    if status == Status::Confirmed {
        let attr = if jobs[index].as_ref().unwrap().ops[0] == Some(id) {
            ATTR_MANUFACTURER
        } else {
            ATTR_MODEL
        };
        let value = read
            .as_ref()
            .and_then(|payload| find_string(payload.as_slice(), attr));
        let Some(value) = value else {
            log::warn!("interview: op={id} confirmed without attr 0x{attr:04x}");
            fail_op(&mut jobs[..], index, id, now);
            return;
        };
        let job = jobs[index].as_mut().unwrap();
        let len = usize::from(value[0]).min(value.len() - 1).min(31);
        match attr {
            ATTR_MANUFACTURER => {
                job.manufacturer[..len].copy_from_slice(&value[1..1 + len]);
                job.man_len = len;
            }
            _ => {
                job.model[..len].copy_from_slice(&value[1..1 + len]);
                job.model_len = len;
            }
        }
        for op in &mut job.ops {
            if *op == Some(id) {
                *op = None;
            }
        }
        if job.man_len == 0 || job.model_len == 0 {
            return;
        }
        let (ext, short, manufacturer, model) = identified(job);
        jobs[index] = None;
        drop(jobs);
        finish(ext, short, manufacturer, model);
        return;
    }
    if !matches!(status, Status::Timeout | Status::Failed) {
        return;
    }
    fail_op(&mut jobs[..], index, id, now);
}

fn fail_op(jobs: &mut [Option<Job>], index: usize, id: u32, now: u64) {
    let job = jobs[index].as_mut().unwrap();
    for op in &mut job.ops {
        if *op == Some(id) {
            *op = None;
        }
    }
    if !job.ops.iter().all(|op| op.is_none()) {
        return;
    }
    job.attempts += 1;
    if job.attempts >= MAX_ATTEMPTS {
        log::warn!("interview: failed for {:016x} after {} attempts", job.ext, job.attempts);
        jobs[index] = None;
    } else {
        job.retry_at = now + RETRY_BACKOFF_MS;
    }
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

fn identified(job: &Job) -> (u64, u16, String, String) {
    let manufacturer = String::from_utf8_lossy(&job.manufacturer[..job.man_len]).to_string();
    let model = String::from_utf8_lossy(&job.model[..job.model_len]).to_string();
    (job.ext, job.short, manufacturer, model)
}

fn finish(ext: u64, short: u16, manufacturer: String, model: String) {
    let interview = z2m_rs::Interview {
        manufacturer: Some(&manufacturer),
        model: &model,
    };
    match z2m_rs::identify(&interview) {
        Some(profile) => {
            log::info!(
                "interview: manufacturer={manufacturer} model={model} profile={} support={:?}",
                profile.id,
                profile.support
            );
            PROFILES.lock().unwrap().insert(ext, profile);
            crate::store::save(ext, short, &manufacturer, &model, profile.id);
        }
        None => log::warn!("interview: unknown manufacturer={manufacturer} model={model}"),
    }
}

pub fn tick(exec: &mut Executor, now: u64) {
    let mut retry: [Option<(u64, u16, bool, bool)>; JOB_SLOTS] = [None; JOB_SLOTS];
    {
        let mut jobs = JOBS.lock().unwrap();
        for (index, slot) in jobs.iter_mut().enumerate() {
            let Some(job) = slot.as_mut() else { continue };
            if job.retry_at != 0 && now >= job.retry_at && job.ops.iter().all(|op| op.is_none()) {
                job.retry_at = 0;
                retry[index] = Some((job.ext, job.short, job.man_len == 0, job.model_len == 0));
            }
        }
    }
    for entry in retry.into_iter().flatten() {
        let (ext, short, need_manufacturer, need_model) = entry;
        let attrs: [(u16, bool); 2] = [(ATTR_MANUFACTURER, need_manufacturer), (ATTR_MODEL, need_model)];
        for (attr, needed) in attrs {
            if !needed {
                continue;
            }
            let request = Request {
                device: ext,
                short,
                endpoint: 1,
                sleepy: true,
                retry: RetryPolicy::SAFE,
                expiry_ms: 10_000,
                timeout_ms: 3_000,
                kind: RequestKind::ReadAttributes { cluster: 0x0000, attribute: attr },
            };
            if let Ok(id) = exec.enqueue(request, now) {
                if let Some(job) = JOBS.lock().unwrap().iter_mut().flatten().find(|job| job.ext == ext) {
                    if attr == ATTR_MANUFACTURER { job.ops[0] = Some(id); } else { job.ops[1] = Some(id); }
                }
            }
        }
    }
}

pub fn selected_profile(eui: u64) -> Option<&'static z2m_rs::Profile> {
    PROFILES.lock().unwrap().get(eui)
}

pub fn restore(eui: u64, profile: &'static z2m_rs::Profile) {
    PROFILES.lock().unwrap().insert(eui, profile);
}
