//! Device lifecycle adapter: C device events and the RAM profile map.
//!
//! The Interview state machine (jobs, retries, cooldown, late-result matching)
//! lives in `zigbee_devices::InterviewManager`; this module only bridges the C
//! callback, the persisted registry and the profile cache.

use std::sync::Mutex;

use zigbee_devices::InterviewManager;
use zigbee_exec::{Executor, OperationResult};

const PROFILE_SLOTS: usize = 8;
const DEVICE_QUEUE: usize = 8;

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
static IDENTIFIED: Mutex<Vec<(u64, u16, &'static z2m_rs::Profile)>> = Mutex::new(Vec::new());
static MANAGER: Mutex<InterviewManager> = Mutex::new(InterviewManager::new());
static CONFIRMED: Mutex<Vec<(u64, zigbee_devices::Confirmed)>> = Mutex::new(Vec::new());

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
    let known = selected_profile(device.0).is_some();
    MANAGER
        .lock()
        .unwrap()
        .start(exec, device.0, device.1, known, now);
}

pub fn on_operation_result(exec: &mut Executor, result: OperationResult, now: u64) {
    let identified = MANAGER.lock().unwrap().on_result(exec, result, now);
    let Some(identified) = identified else {
        return;
    };
    log::info!(
        "interview: manufacturer={} model={} profile={} support={:?}",
        identified.manufacturer,
        identified.model,
        identified.profile.id,
        identified.profile.support
    );
    PROFILES
        .lock()
        .unwrap()
        .insert(identified.eui, identified.profile);
    crate::store::save(
        identified.eui,
        identified.short,
        &identified.manufacturer,
        &identified.model,
        identified.profile.id,
        &identified.descriptor,
    );
    crate::configure::ensure(exec, identified.eui, identified.short, now);
    let mut queue = IDENTIFIED.lock().unwrap();
    if queue.len() < 8 {
        queue.push((identified.eui, identified.short, identified.profile));
    }
}

pub fn take_identified() -> Option<(u64, u16, &'static z2m_rs::Profile)> {
    IDENTIFIED.lock().unwrap().pop()
}

pub fn tick(exec: &mut Executor, now: u64) {
    MANAGER.lock().unwrap().tick(exec, now);
}

pub fn confirm_attribute(eui: u64, endpoint: u8, cluster: u16, attribute: u16) {
    let mut confirmed = CONFIRMED.lock().unwrap();
    if let Some((_, state)) = confirmed.iter_mut().find(|(id, _)| *id == eui) {
        state.confirm(endpoint, cluster, attribute);
        return;
    }
    if confirmed.len() < 8 {
        let mut state = zigbee_devices::Confirmed::new();
        state.confirm(endpoint, cluster, attribute);
        confirmed.push((eui, state));
    }
}

pub fn confirmed(eui: u64) -> zigbee_devices::Confirmed {
    CONFIRMED.lock().unwrap().iter()
        .find(|(id, _)| *id == eui)
        .map(|(_, state)| *state)
        .unwrap_or_else(zigbee_devices::Confirmed::new)
}

pub fn is_attribute_confirmed(eui: u64, endpoint: u8, cluster: u16, attribute: u16) -> bool {
    CONFIRMED.lock().unwrap().iter().any(|(id, state)| {
        *id == eui && state.is_confirmed(endpoint, cluster, attribute)
    })
}

pub fn selected_profile(eui: u64) -> Option<&'static z2m_rs::Profile> {
    PROFILES.lock().unwrap().get(eui)
}

pub fn restore(eui: u64, profile: &'static z2m_rs::Profile) {
    PROFILES.lock().unwrap().insert(eui, profile);
}
