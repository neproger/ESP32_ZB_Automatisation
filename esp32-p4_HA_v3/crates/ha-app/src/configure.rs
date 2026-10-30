//! Configure lifecycle adapter: one plan at a time, pending devices queued.
//!
//! `zigbee_devices::ConfigRun` owns step/timeout/retry logic; this module only
//! decides what to configure, when, and records the result.

use std::sync::Mutex;

use zigbee_devices::{ConfigRun, RunOptions};
use zigbee_exec::Executor;

const FIRST_APP_ENDPOINT: u8 = 1;
const STANDARD_CONFIG_VERSION: u16 = 4;
const COOLDOWN_MS: u64 = 30_000;
const COOLDOWN_SLOTS: usize = crate::store::MAX_DEVICES;
const REFRESH_SLOTS: usize = crate::store::MAX_DEVICES;
const PENDING_SLOTS: usize = crate::store::MAX_DEVICES;

struct Pending {
    eui: u64,
    short: u16,
    endpoint: u8,
    definition: &'static str,
    steps: Vec<z2m_rs::ConfigureStep>,
    options: RunOptions,
}

static RUN: Mutex<Option<ConfigRun>> = Mutex::new(None);
static PENDING: Mutex<Vec<Pending>> = Mutex::new(Vec::new());
static COOLDOWN: Mutex<[(u64, u64); COOLDOWN_SLOTS]> = Mutex::new([(0, 0); COOLDOWN_SLOTS]);
static REFRESHED: Mutex<[Option<u64>; REFRESH_SLOTS]> = Mutex::new([None; REFRESH_SLOTS]);

pub fn ensure(exec: &mut Executor, eui: u64, short: u16, now: u64) {
    if in_cooldown(eui, now) || active_eui(eui) || pending_has(eui) {
        return;
    }
    let Some(profile) = crate::interview::selected_profile(eui) else {
        return;
    };
    if profile.id == zigbee_devices::STANDARD_PROFILE_ID {
        queue_standard(eui, short, now);
        start_next(exec, now);
        return;
    }
    let Some(plan) = profile.configure else {
        return;
    };
    if crate::store::is_configured(eui, profile.id, plan.version) {
        return;
    }
    if plan.steps.is_empty() {
        crate::store::mark_configured(eui, profile.id, plan.version);
        return;
    }
    queue(Pending {
        eui,
        short,
        endpoint: FIRST_APP_ENDPOINT,
        definition: profile.id,
        steps: plan.steps.to_vec(),
        options: RunOptions::from_plan(plan),
    });
    start_next(exec, now);
}

fn push_bind(steps: &mut Vec<z2m_rs::ConfigureStep>, endpoint: u8, cluster: u16) {
    steps.push(z2m_rs::ConfigureStep::Bind {
        endpoint: z2m_rs::ConfigureEndpoint::Id(endpoint),
        cluster,
    });
}

fn push_read(
    steps: &mut Vec<z2m_rs::ConfigureStep>,
    endpoint: u8,
    cluster: u16,
    attributes: &'static [u16],
) {
    steps.push(z2m_rs::ConfigureStep::Read {
        endpoint: z2m_rs::ConfigureEndpoint::Id(endpoint),
        cluster,
        attributes,
    });
}

fn push_report(
    steps: &mut Vec<z2m_rs::ConfigureStep>,
    endpoint: u8,
    cluster: u16,
    attribute: u16,
    attribute_type: u8,
    min: u16,
    max: u16,
    change: u32,
) {
    steps.push(z2m_rs::ConfigureStep::Report {
        endpoint: z2m_rs::ConfigureEndpoint::Id(endpoint),
        cluster,
        attribute,
        attribute_type,
        min,
        max,
        change,
    });
}

fn standard_steps(
    descriptor: &zigbee_devices::DeviceDescriptor,
    configure: bool,
) -> Vec<z2m_rs::ConfigureStep> {
    let mut steps = Vec::new();
    for endpoint in descriptor.endpoints() {
        let ep = endpoint.endpoint;
        if endpoint.has_server(zigbee_devices::standard::ONOFF) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::ONOFF);
            }
            push_read(&mut steps, ep, zigbee_devices::standard::ONOFF, &[0x0000]);
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::ONOFF,
                    0x0000,
                    0x10,
                    0,
                    1,
                    1,
                );
            }
        }
        if endpoint.has_server(zigbee_devices::standard::LEVEL) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::LEVEL);
            }
            push_read(&mut steps, ep, zigbee_devices::standard::LEVEL, &[0x0000]);
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::LEVEL,
                    0x0000,
                    0x20,
                    0,
                    1,
                    1,
                );
            }
        }
        if endpoint.has_server(zigbee_devices::standard::COLOR) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::COLOR);
            }
            push_read(
                &mut steps,
                ep,
                zigbee_devices::standard::COLOR,
                &[0x0007, 0x0003, 0x0004],
            );
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::COLOR,
                    0x0007,
                    0x21,
                    1,
                    3600,
                    10,
                );
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::COLOR,
                    0x0003,
                    0x21,
                    1,
                    3600,
                    1,
                );
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::COLOR,
                    0x0004,
                    0x21,
                    1,
                    3600,
                    1,
                );
            }
        }
        if endpoint.has_server(zigbee_devices::standard::POWER_CONFIG) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::POWER_CONFIG);
            }
            push_read(
                &mut steps,
                ep,
                zigbee_devices::standard::POWER_CONFIG,
                &[0x0021, 0x0020],
            );
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::POWER_CONFIG,
                    0x0021,
                    0x20,
                    3600,
                    3600,
                    10,
                );
            }
        }
        if endpoint.has_server(zigbee_devices::standard::TEMPERATURE) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::TEMPERATURE);
            }
            push_read(
                &mut steps,
                ep,
                zigbee_devices::standard::TEMPERATURE,
                &[0x0000],
            );
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::TEMPERATURE,
                    0x0000,
                    0x29,
                    10,
                    3600,
                    100,
                );
            }
        }
        if endpoint.has_server(zigbee_devices::standard::HUMIDITY) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::HUMIDITY);
            }
            push_read(
                &mut steps,
                ep,
                zigbee_devices::standard::HUMIDITY,
                &[0x0000],
            );
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::HUMIDITY,
                    0x0000,
                    0x21,
                    10,
                    3600,
                    100,
                );
            }
        }
        if endpoint.has_server(zigbee_devices::standard::ILLUMINANCE) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::ILLUMINANCE);
            }
            push_read(
                &mut steps,
                ep,
                zigbee_devices::standard::ILLUMINANCE,
                &[0x0000],
            );
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::ILLUMINANCE,
                    0x0000,
                    0x21,
                    10,
                    3600,
                    5,
                );
            }
        }
        if endpoint.has_server(zigbee_devices::standard::OCCUPANCY) {
            if configure {
                push_bind(&mut steps, ep, zigbee_devices::standard::OCCUPANCY);
            }
            push_read(
                &mut steps,
                ep,
                zigbee_devices::standard::OCCUPANCY,
                &[0x0000],
            );
            if configure {
                push_report(
                    &mut steps,
                    ep,
                    zigbee_devices::standard::OCCUPANCY,
                    0x0000,
                    0x18,
                    1,
                    3600,
                    0,
                );
            }
        }
    }
    steps
}

fn step_endpoint(step: &z2m_rs::ConfigureStep, first: u8) -> u8 {
    match step {
        z2m_rs::ConfigureStep::Read { endpoint, .. }
        | z2m_rs::ConfigureStep::Bind { endpoint, .. }
        | z2m_rs::ConfigureStep::Report { endpoint, .. } => match endpoint {
            z2m_rs::ConfigureEndpoint::Id(endpoint) => *endpoint,
            z2m_rs::ConfigureEndpoint::First => first,
        },
    }
}

fn queue_standard(eui: u64, short: u16, now: u64) {
    if refreshed(eui) || in_cooldown(eui, now) || pending_has(eui) || active_eui(eui) {
        return;
    }
    let Some(descriptor) = crate::store::descriptor(eui) else {
        return;
    };
    let Some(first) = descriptor.endpoints().next() else {
        return;
    };
    let configured = crate::store::is_configured(
        eui,
        zigbee_devices::STANDARD_PROFILE_ID,
        STANDARD_CONFIG_VERSION,
    );
    let steps = standard_steps(&descriptor, !configured);
    if steps.is_empty() {
        return;
    }
    queue(Pending {
        eui,
        short,
        endpoint: first.endpoint,
        definition: zigbee_devices::STANDARD_PROFILE_ID,
        steps,
        options: RunOptions {
            version: STANDARD_CONFIG_VERSION,
            timeout_ms: 5_000,
            retries: 2,
            optional_reads: true,
            optional_reports: true,
        },
    });
}

fn queue(pending: Pending) {
    let mut queue = PENDING.lock().unwrap();
    if queue.len() >= PENDING_SLOTS {
        log::warn!("configure: pending queue full, {} skipped", pending.eui);
        return;
    }
    queue.push(pending);
}

/// Drop any queued/active plan for a removed device and clear its cooldown.
pub fn forget(eui: u64) {
    {
        let mut run = RUN.lock().unwrap();
        if run.as_ref().is_some_and(|run| run.eui() == eui) {
            *run = None;
        }
    }
    PENDING.lock().unwrap().retain(|pending| pending.eui != eui);
    for entry in COOLDOWN.lock().unwrap().iter_mut() {
        if entry.0 == eui {
            *entry = (0, 0);
        }
    }
    if let Some(slot) = REFRESHED
        .lock()
        .unwrap()
        .iter_mut()
        .find(|entry| **entry == Some(eui))
    {
        *slot = None;
    }
}

fn pending_has(eui: u64) -> bool {
    PENDING
        .lock()
        .unwrap()
        .iter()
        .any(|pending| pending.eui == eui)
}

fn active_eui(eui: u64) -> bool {
    RUN.lock()
        .unwrap()
        .as_ref()
        .is_some_and(|run| run.eui() == eui)
}

fn start_next(exec: &mut Executor, now: u64) {
    let mut run = RUN.lock().unwrap();
    if run.is_some() {
        return;
    }
    let mut queue = PENDING.lock().unwrap();
    if queue.is_empty() {
        return;
    }
    let pending = queue.remove(0);
    drop(queue);
    *run = Some(ConfigRun::new(
        exec,
        (pending.eui, pending.short),
        pending.endpoint,
        pending.definition,
        &pending.steps,
        pending.options,
        now,
    ));
}

pub fn on_result(exec: &mut Executor, result: zigbee_exec::OperationResult, now: u64) {
    let outcome = {
        let mut guard = RUN.lock().unwrap();
        let Some(run) = guard.as_mut() else {
            return;
        };
        if run.accepts(result.id) && result.status == zigbee_exec::Status::Confirmed {
            if let Some(
                step @ z2m_rs::ConfigureStep::Read {
                    cluster,
                    attributes,
                    ..
                },
            ) = run.current_step()
            {
                let endpoint = step_endpoint(&step, run.endpoint());
                let present = result.read.as_ref().and_then(|payload| {
                    z2m_rs::AttributeFrameView::parse(cluster, endpoint, 0x0A, payload.as_slice())
                        .ok()
                });
                let Some(present) = present else {
                    log::warn!(
                        "configure: read response eui={:016x} ep={endpoint} cluster=0x{cluster:04x} is not usable",
                        run.eui()
                    );
                    run.on_result(result);
                    return;
                };
                for attribute in attributes {
                    if present.attributes().any(|entry| entry.id == *attribute) {
                        crate::interview::confirm_attribute(
                            run.eui(),
                            endpoint,
                            cluster,
                            *attribute,
                        );
                    }
                }
                if let Some(descriptor) = crate::store::descriptor(run.eui()) {
                    if let Some(endpoint_descriptor) = descriptor.endpoint(endpoint) {
                        let confirmed = crate::interview::confirmed(run.eui());
                        if !crate::capabilities::push_capabilities(
                            zigbee_devices::zigbee_entity_id(run.eui(), endpoint),
                            zigbee_devices::standard::capabilities_confirmed(
                                endpoint_descriptor,
                                &confirmed,
                            ),
                        ) {
                            log::warn!(
                                "configure: capability mailbox full eui={:016x} ep={endpoint}",
                                run.eui()
                            );
                        }
                    }
                }
            }
        }
        run.on_result(result);
        advance(&mut guard, exec, now)
    };
    finish_outcome(outcome, now);
    start_next(exec, now);
}

pub fn tick(exec: &mut Executor, now: u64) {
    let outcome = {
        let mut guard = RUN.lock().unwrap();
        advance(&mut guard, exec, now)
    };
    finish_outcome(outcome, now);
    start_next(exec, now);
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

fn finish_outcome(outcome: Option<(u64, &'static str, u16, bool)>, now: u64) {
    let Some((eui, definition, version, failed)) = outcome else {
        return;
    };
    if failed {
        log::warn!(
            "configure: failed eui={eui:016x} profile={definition} version={version}; cooldown"
        );
        set_cooldown(eui, now);
        return;
    }
    if definition == zigbee_devices::STANDARD_PROFILE_ID {
        mark_refreshed(eui);
    }
    crate::store::mark_configured(eui, definition, version);
}

fn refreshed(eui: u64) -> bool {
    REFRESHED
        .lock()
        .unwrap()
        .iter()
        .any(|entry| *entry == Some(eui))
}

fn mark_refreshed(eui: u64) {
    let mut refreshed = REFRESHED.lock().unwrap();
    if refreshed.iter().any(|entry| *entry == Some(eui)) {
        return;
    }
    if let Some(slot) = refreshed.iter_mut().find(|entry| entry.is_none()) {
        *slot = Some(eui);
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
