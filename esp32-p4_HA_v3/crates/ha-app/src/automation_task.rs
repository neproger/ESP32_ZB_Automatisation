//! Automation subscriber task.
//!
//! The engine owns its inbox and never runs in the Domain producer or the
//! Dispatcher. It reads current state from Domain and posts canonical commands
//! through Domain.post, so the adapter executor receives them from Domain.

use std::sync::mpsc::Receiver;

use ha_domain::canonical::{
    StateKey, COMMAND_PROPERTY, ENTITY_AUTOMATION, ENTITY_PROPERTY_STATE, ENTITY_SYSTEM,
    EVENT_MINUTE_TICK, PROPERTY_SYSTEM_MINUTES_OF_DAY, PROPERTY_SYSTEM_WEEKDAY, SOURCE_AUTOMATION,
    STATE_RECORD_LEN,
};
use ha_domain::{Domain, DomainHandle, Fact, FactKind};

struct State<'a>(&'a Domain);

impl ha_automation::StateSource for State<'_> {
    fn state(&self, entity: u64, property: u32) -> Option<f64> {
        let key = StateKey { entity, property }.to_bytes();
        let mut bytes = [0u8; STATE_RECORD_LEN];
        self.0
            .entity_get(ENTITY_PROPERTY_STATE, &key, &mut bytes)
            .ok()?;
        let value = ha_domain::Value::from_bytes(&bytes)?;
        ha_automation::value_to_f64(&value)
    }
}

fn reload(domain: &Domain, engine: &mut ha_automation::RuleEngine) {
    engine.clear();
    if let Err(error) = domain.entity_iter(ENTITY_AUTOMATION, |key, record| {
        if key.len() == 8 && record.len() == ha_domain::canonical::AUTOMATION_RECORD_LEN {
            let id = u64::from_le_bytes(key.try_into().expect("automation key"));
            engine.add_rule(id, record);
        }
    }) {
        log::warn!("automation: rebuild failed: {error:?}");
    }
    engine.build_index();
    log::info!(
        "automation: rules={} index={}",
        engine.rule_count(),
        engine.index_count()
    );
}

fn emit(domain: &DomainHandle, actions: &ha_automation::Actions) {
    if let Err(error) = domain.with_mut(|domain| {
        for command in actions.iter() {
            let bytes = command.encode();
            let meta = ha_domain::FactMeta {
                source: SOURCE_AUTOMATION as u8,
                value: ha_domain::Value::NONE,
            };
            if let Err(error) = domain.post(COMMAND_PROPERTY, &bytes, None, meta) {
                log::warn!(
                    "automation: command rejected entity={:016x} property={}: {error:?}",
                    command.entity,
                    command.property
                );
            }
        }
    }) {
        log::warn!("automation: command lane unavailable: {error:?}");
    }
}

fn system_minute(domain: &Domain) -> Option<(u16, u8)> {
    let minutes = <State<'_> as ha_automation::StateSource>::state(
        &State(domain),
        ENTITY_SYSTEM,
        PROPERTY_SYSTEM_MINUTES_OF_DAY,
    )?;
    let weekday = <State<'_> as ha_automation::StateSource>::state(
        &State(domain),
        ENTITY_SYSTEM,
        PROPERTY_SYSTEM_WEEKDAY,
    )?;
    Some((
        minutes.round().clamp(0.0, 1439.0) as u16,
        weekday.round().clamp(0.0, 6.0) as u8,
    ))
}

fn handle_fact(domain: &DomainHandle, engine: &mut ha_automation::RuleEngine, fact: Fact) {
    match fact.kind {
        FactKind::EntityUpserted | FactKind::EntityRemoved if fact.entity == ENTITY_AUTOMATION => {
            domain.with(|guard| reload(guard, engine));
        }
        FactKind::EntityUpserted if fact.entity == ENTITY_PROPERTY_STATE => {
            let key = fact.key();
            if key.len() < 16 {
                return;
            }
            let entity = u64::from_le_bytes(key[..8].try_into().expect("state entity"));
            let property = u32::from_le_bytes(key[8..12].try_into().expect("state property"));
            let Some(value) = ha_automation::value_to_f64(&fact.value) else {
                return;
            };
            let mut actions = ha_automation::Actions::new();
            domain.with(|guard| {
                engine.on_state(entity, property, value, &State(guard), &mut actions);
            });
            emit(domain, &actions);
        }
        FactKind::Event => {
            let key = fact.key();
            if key.len() < 8 {
                return;
            }
            let entity = u64::from_le_bytes(key[..8].try_into().expect("event entity"));
            let Some(event) = fact.value.as_u32() else {
                return;
            };
            let mut actions = ha_automation::Actions::new();
            domain.with(|guard| {
                if entity == ENTITY_SYSTEM && event == EVENT_MINUTE_TICK {
                    if let Some((minutes, weekday)) = system_minute(guard) {
                        engine.on_minute(minutes, 1u8 << weekday, &State(guard), &mut actions);
                    }
                } else {
                    engine.on_event(entity, event, &State(guard), &mut actions);
                }
            });
            emit(domain, &actions);
        }
        _ => {}
    }
}

pub fn spawn(domain: DomainHandle, facts: Receiver<Fact>) {
    let mut engine = ha_automation::RuleEngine::boxed();
    domain.with(|guard| reload(guard, &mut engine));
    std::thread::Builder::new()
        .name("automation".into())
        .stack_size(8192)
        .spawn(move || {
            while let Ok(fact) = facts.recv() {
                handle_fact(&domain, &mut engine, fact);
            }
        })
        .expect("automation thread");
}
