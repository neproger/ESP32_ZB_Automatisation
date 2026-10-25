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
    let Some(profile) = crate::interview::selected_profile(eui) else { return; };
    if profile.id == "standard.zcl" {
        start_standard(exec, eui, short, now);
        return;
    }
    let Some(plan) = profile.configure else { return; };
    if crate::store::is_configured(eui, profile.id, plan.version) { return; }
    if in_cooldown(eui, now) { return; }
    let mut run = RUN.lock().unwrap();
    if run.is_some() { return; }
    *run = Some(ConfigRun::start(exec, (eui, short), FIRST_APP_ENDPOINT, profile.id, plan, now));
}

fn standard_steps(
    endpoint: &zigbee_devices::EndpointDescriptor,
 ) -> ([z2m_rs::ConfigureStep; 32], usize) {
    use z2m_rs::{ConfigureEndpoint, ConfigureStep};
    let placeholder = ConfigureStep::Bind { endpoint: ConfigureEndpoint::First, cluster: 0 };
    let mut steps = [placeholder; 32];
    let mut count = 0;
    let mut bind = |cluster: u16, steps: &mut [ConfigureStep; 32], count: &mut usize| {
        if *count < steps.len() {
            steps[*count] = ConfigureStep::Bind { endpoint: ConfigureEndpoint::First, cluster };
            *count += 1;
        }
    };
    let mut read = |cluster: u16, attributes: &'static [u16], steps: &mut [ConfigureStep; 32], count: &mut usize| {
        if *count < steps.len() {
            steps[*count] = ConfigureStep::Read { endpoint: ConfigureEndpoint::First, cluster, attributes };
            *count += 1;
        }
    };
    let mut report = |cluster: u16, attribute: u16, ty: u8, min: u16, max: u16, change: u32,
                      steps: &mut [ConfigureStep; 32], count: &mut usize| {
        if *count < steps.len() {
            steps[*count] = ConfigureStep::Report {
                endpoint: ConfigureEndpoint::First,
                cluster,
                attribute,
                attribute_type: ty,
                min,
                max,
                change,
            };
            *count += 1;
        }
    };
    if endpoint.has_server(0x0006) {
        bind(0x0006, &mut steps, &mut count);
        read(0x0006, &[0x0000], &mut steps, &mut count);
        report(0x0006, 0x0000, 0x10, 0, 3600, 1, &mut steps, &mut count);
    }
    if endpoint.has_server(0x0008) {
        bind(0x0008, &mut steps, &mut count);
        read(0x0008, &[0x0000], &mut steps, &mut count);
        report(0x0008, 0x0000, 0x20, 1, 3600, 1, &mut steps, &mut count);
    }
    if endpoint.has_server(0x0001) {
        bind(0x0001, &mut steps, &mut count);
        read(0x0001, &[0x0021, 0x0020], &mut steps, &mut count);
        report(0x0001, 0x0021, 0x20, 3600, 3600, 10, &mut steps, &mut count);
    }
    if endpoint.has_server(0x0402) {
        bind(0x0402, &mut steps, &mut count);
        read(0x0402, &[0x0000], &mut steps, &mut count);
        report(0x0402, 0x0000, 0x29, 10, 3600, 100, &mut steps, &mut count);
    }
    if endpoint.has_server(0x0405) {
        bind(0x0405, &mut steps, &mut count);
        read(0x0405, &[0x0000], &mut steps, &mut count);
        report(0x0405, 0x0000, 0x21, 10, 3600, 100, &mut steps, &mut count);
    }
    if endpoint.has_server(0x0400) {
        bind(0x0400, &mut steps, &mut count);
        read(0x0400, &[0x0000], &mut steps, &mut count);
        report(0x0400, 0x0000, 0x21, 10, 3600, 5, &mut steps, &mut count);
    }
    if endpoint.has_server(0x0406) {
        bind(0x0406, &mut steps, &mut count);
        read(0x0406, &[0x0000], &mut steps, &mut count);
        report(0x0406, 0x0000, 0x18, 1, 3600, 0, &mut steps, &mut count);
    }
    (steps, count)
}

fn start_standard(exec: &mut Executor, eui: u64, short: u16, now: u64) {
    let Some(descriptor) = crate::store::descriptor(eui) else { return; };
    let Some(endpoint) = descriptor.endpoint(FIRST_APP_ENDPOINT) else { return; };
    let (steps, count) = standard_steps(endpoint);
    if count == 0 || crate::store::is_configured(eui, "standard.zcl", 1) || in_cooldown(eui, now) {
        return;
    }
    let mut run = RUN.lock().unwrap();
    if run.is_some() { return; }
    *run = Some(ConfigRun::start_owned(
        exec,
        (eui, short),
        FIRST_APP_ENDPOINT,
        "standard.zcl",
        &steps[..count],
        1,
        now,
    ));
}

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
    if run.accepts(result.id) && result.status == zigbee_exec::Status::Confirmed {
        if let Some(z2m_rs::ConfigureStep::Read { cluster, attributes, .. }) = run.current_step() {
            for attribute in attributes {
                crate::interview::confirm_attribute(run.eui(), FIRST_APP_ENDPOINT, cluster, *attribute);
            }
            if let Some(descriptor) = crate::store::descriptor(run.eui()) {
                if let Some(endpoint) = descriptor.endpoint(FIRST_APP_ENDPOINT) {
                    let confirmed = crate::interview::confirmed(run.eui());
                    crate::canonical::push_capabilities(
                        zigbee_devices::zigbee_entity_id(run.eui(), FIRST_APP_ENDPOINT),
                        zigbee_devices::standard::capabilities_confirmed(endpoint, &confirmed),
                    );
                }
            }
        }
    }
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
