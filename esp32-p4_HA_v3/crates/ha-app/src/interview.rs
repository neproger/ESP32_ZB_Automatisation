//! Minimal Device Interview: Basic cluster read -> `z2m-rs::identify`.
//!
//! The interview is a protocol operation, not part of `z2m-rs`: it collects the
//! signature (manufacturer/model), then asks the independent library for a
//! profile. Reads go through `zigbee-exec` like any other operation.

use std::sync::Mutex;

use zigbee_exec::{Executor, Request, RequestKind, RetryPolicy};

const ATTR_MANUFACTURER: u16 = 0x0004;
const ATTR_MODEL: u16 = 0x0005;

struct State {
    ext: u64,
    short: u16,
    manufacturer: [u8; 32],
    man_len: usize,
    model: [u8; 32],
    model_len: usize,
    waiting: u8,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);
const PROFILE_SLOTS: usize = 8;

#[derive(Clone, Copy)]
struct ProfileMap {
    items: [Option<(u64, &'static z2m_rs::Profile)>; PROFILE_SLOTS],
}

impl ProfileMap {
    const fn new() -> Self {
        Self { items: [None; PROFILE_SLOTS] }
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

const DEVICE_QUEUE: usize = 8;

struct DeviceQueue {
    items: [Option<(u64, u16)>; DEVICE_QUEUE],
    head: usize,
    tail: usize,
}

impl DeviceQueue {
    const fn new() -> Self {
        Self { items: [None; DEVICE_QUEUE], head: 0, tail: 0 }
    }

    fn push(&mut self, ext: u64, short: u16) {
        if self.items.iter().flatten().any(|queued| queued.0 == ext) {
            return;
        }
        if self.tail - self.head >= DEVICE_QUEUE {
            log::warn!("interview: device queue full, dropping {:016x}", ext);
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

unsafe extern "C" fn on_read(
    ext: u64,
    short: u16,
    _ep: u8,
    cluster: u16,
    attr: u16,
    _ty: u8,
    value: *const u8,
    len: u16,
) {
    if cluster != 0x0000 || value.is_null() || len == 0 {
        return;
    }
    let bytes = std::slice::from_raw_parts(value, usize::from(len));
    handle_read(ext, short, attr, bytes);
}

extern "C" {
    fn zb_poc_set_device_event_cb(cb: Option<unsafe extern "C" fn(u64, u16)>);
    fn zb_interview_set_callback(
        cb: Option<unsafe extern "C" fn(u64, u16, u8, u16, u16, u8, *const u8, u16)>,
    );
}

pub fn install() {
    unsafe { zb_interview_set_callback(Some(on_read)) };
    unsafe { zb_poc_set_device_event_cb(Some(on_device)) };
    log::info!("interview + device event callbacks installed");
}

pub fn start(exec: &mut Executor, device: (u64, u16), now: u64) {
    if device.0 == 0 || selected_profile(device.0).is_some() {
        return;
    }
    {
        let mut state = STATE.lock().unwrap();
        if state.is_some() {
            return;
        }
        *state = Some(State {
            ext: device.0,
            short: device.1,
            manufacturer: [0; 32],
            man_len: 0,
            model: [0; 32],
            model_len: 0,
            waiting: 2,
        });
    }
    for (attr, label) in [(ATTR_MANUFACTURER, "manufacturer"), (ATTR_MODEL, "model")] {
        let request = Request {
            device: device.0,
            short: device.1,
            endpoint: 1,
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
            Ok(id) => log::info!("interview: read {label} op={id}"),
            Err(error) => log::warn!("interview: {label} not queued: {error:?}"),
        }
    }
}

pub fn selected_profile(eui: u64) -> Option<&'static z2m_rs::Profile> {
    PROFILES.lock().unwrap().get(eui)
}

pub fn restore(eui: u64, profile: &'static z2m_rs::Profile) {
    PROFILES.lock().unwrap().insert(eui, profile);
}

fn handle_read(ext: u64, short: u16, attr: u16, bytes: &[u8]) {
    let (manufacturer, model, done) = {
        let mut guard = STATE.lock().unwrap();
        let Some(state) = guard.as_mut() else {
            return;
        };
        if state.ext != ext || bytes.is_empty() {
            return;
        }
        let len = usize::from(bytes[0]).min(bytes.len() - 1).min(31);
        match attr {
            ATTR_MANUFACTURER => {
                state.manufacturer[..len].copy_from_slice(&bytes[1..1 + len]);
                state.man_len = len;
            }
            ATTR_MODEL => {
                state.model[..len].copy_from_slice(&bytes[1..1 + len]);
                state.model_len = len;
            }
            _ => return,
        }
        state.waiting = state.waiting.saturating_sub(1);
        let done = state.waiting == 0 && state.man_len > 0 && state.model_len > 0;
        let manufacturer = String::from_utf8_lossy(&state.manufacturer[..state.man_len]).to_string();
        let model = String::from_utf8_lossy(&state.model[..state.model_len]).to_string();
        (manufacturer, model, done)
    };

    if !done {
        return;
    }
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
    *STATE.lock().unwrap() = None;
}
