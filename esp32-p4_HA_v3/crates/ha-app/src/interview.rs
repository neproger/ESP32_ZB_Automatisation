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
static PROFILE: Mutex<Option<&'static z2m_rs::Profile>> = Mutex::new(None);
static DONE: Mutex<bool> = Mutex::new(false);

unsafe extern "C" fn on_read(
    ext: u64,
    _short: u16,
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
    handle_read(ext, attr, bytes);
}

extern "C" {
    fn zb_interview_set_callback(
        cb: Option<unsafe extern "C" fn(u64, u16, u8, u16, u16, u8, *const u8, u16)>,
    );
}

pub fn install() {
    unsafe { zb_interview_set_callback(Some(on_read)) };
    log::info!("interview callback installed");
}

pub fn start(exec: &mut Executor, device: (u64, u16), now: u64) {
    if device.0 == 0 || *DONE.lock().unwrap() {
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
            sleepy: false,
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

pub fn selected_profile() -> Option<&'static z2m_rs::Profile> {
    *PROFILE.lock().unwrap()
}

fn handle_read(ext: u64, attr: u16, bytes: &[u8]) {
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
            *PROFILE.lock().unwrap() = Some(profile);
        }
        None => log::warn!("interview: unknown manufacturer={manufacturer} model={model}"),
    }
    *STATE.lock().unwrap() = None;
    *DONE.lock().unwrap() = true;
}
