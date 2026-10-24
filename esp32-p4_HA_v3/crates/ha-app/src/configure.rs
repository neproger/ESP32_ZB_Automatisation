//! Configure lifecycle adapter: one plan at a time over the verified `ConfigRun`.

use std::sync::Mutex;

use zigbee_exec::Executor;

use crate::exec::ConfigRun;

const FIRST_APP_ENDPOINT: u8 = 1;
const COOLDOWN_MS: u64 = 30_000;
const COOLDOWN_SLOTS: usize = 8;

static RUN: Mutex<Option<ConfigRun>> = Mutex::new(None);
static COOLDOWN: Mutex<[(u64, u64); COOLDOWN_SLOTS]> = Mutex::new([(0, 0); COOLDOWN_SLOTS]);

pub fn ensure(exec: &mut Executor, eui: u64, short: u16, now: u64) {
    let Some(profile) = crate::interview::selected_profile(eui) else {
        return;
    };
    let Some(plan) = profile.configure else {
        return;
    };
    if crate::store::is_configured(eui, profile.id, plan.version) {
        log::info!(
            "configure: skip eui={eui:016x} profile={} version={} (configured)",
            profile.id,
            plan.version
        );
        return;
    }
    if in_cooldown(eui, now) {
        return;
    }
    let mut run = RUN.lock().unwrap();
    if run.is_some() {
        return;
    }
    log::info!(
        "configure: queued profile={} version={} eui={eui:016x}",
        profile.id,
        plan.version
    );
    *run = Some(ConfigRun::start(
        exec,
        (eui, short),
        FIRST_APP_ENDPOINT,
        profile.id,
        plan,
        now,
    ));
}

pub fn on_result(exec: &mut Executor, result: zigbee_exec::OperationResult, now: u64) {
    let mut guard = RUN.lock().unwrap();
    let Some(run) = guard.as_mut() else {
        return;
    };
    run.on_result(result);
    let Some((eui, definition, version, failed)) = advance(&mut guard, exec, now) else {
        return;
    };
    finish(eui, definition, version, failed, now);
}

pub fn tick(exec: &mut Executor, now: u64) {
    let mut guard = RUN.lock().unwrap();
    let Some((eui, definition, version, failed)) = advance(&mut guard, exec, now) else {
        return;
    };
    finish(eui, definition, version, failed, now);
}

fn advance(
    run: &mut Option<ConfigRun>,
    exec: &mut Executor,
    now: u64,
) -> Option<(u64, &'static str, u16, bool)> {
    let active = run.as_mut()?;
    if !active.tick(exec, now) {
        return None;
    }
    let outcome = (
        active.eui(),
        active.definition(),
        active.version(),
        active.failed(),
    );
    *run = None;
    Some(outcome)
}

fn finish(eui: u64, definition: &'static str, version: u16, failed: bool, now: u64) {
    if failed {
        log::warn!(
            "configure: failed eui={eui:016x} profile={definition} version={version}; cooldown"
        );
        set_cooldown(eui, now);
    } else {
        crate::store::mark_configured(eui, definition, version);
    }
}

fn in_cooldown(eui: u64, now: u64) -> bool {
    COOLDOWN
        .lock()
        .unwrap()
        .iter()
        .any(|(cooldown_eui, until)| *cooldown_eui == eui && now < *until)
}

fn set_cooldown(eui: u64, now: u64) {
    let until = now + COOLDOWN_MS;
    let mut cooldown = COOLDOWN.lock().unwrap();
    if let Some(entry) = cooldown
        .iter_mut()
        .find(|(cooldown_eui, until)| *cooldown_eui == eui || now >= *until)
    {
        *entry = (eui, until);
        return;
    }
    if let Some(entry) = cooldown.iter_mut().min_by_key(|(_, until)| *until) {
        *entry = (eui, until);
    }
}
