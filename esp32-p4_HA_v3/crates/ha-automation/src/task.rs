//! Automation subscriber task.
//!
//! The engine owns its inbox and never runs in the Domain producer or the
//! Dispatcher. It reads current state from Domain and posts canonical commands
//! through Domain.post, so the adapter executor receives them from Domain.

use std::sync::mpsc::{sync_channel, Receiver};

use ha_domain::canonical::{
    StateKey, COMMAND_PROPERTY, ENTITY_AUTOMATION, ENTITY_ENTITY, ENTITY_PROPERTY_STATE,
    ENTITY_SYSTEM, EVENT_MINUTE_TICK, PROPERTY_SYSTEM_MINUTES_OF_DAY, PROPERTY_SYSTEM_WEEKDAY_MASK,
    SOURCE_AUTOMATION, STATE_KEY_LEN, STATE_RECORD_LEN,
};
use ha_domain::{DomainHandle, Fact, FactKind, Filter};

const INBOX_CAPACITY: usize = 32;
const TASK_STACK: usize = 8192;

struct State<'a>(&'a DomainHandle);

impl crate::StateSource for State<'_> {
    fn state(&self, entity: u64, property: u32) -> Option<f64> {
        let key = StateKey { entity, property }.to_bytes();
        let mut bytes = [0u8; STATE_RECORD_LEN];
        self.0
            .entity_get(ENTITY_PROPERTY_STATE, &key, &mut bytes)
            .ok()?;
        let value = ha_domain::Value::from_bytes(&bytes)?;
        crate::value_to_f64(&value)
    }
}

fn subscription(kind_mask: u32, entity: u32) -> Filter {
    Filter {
        kind_mask,
        source_mask: 0,
        entity,
    }
}

fn reload(domain: &DomainHandle, engine: &mut crate::RuleEngine) {
    engine.clear();
    match domain.entity_snapshot(ENTITY_AUTOMATION) {
        Ok(snapshot) => {
            for index in 0..snapshot.len() {
                let Some(key) = snapshot.key(index) else {
                    continue;
                };
                let Some(record) = snapshot.record(index) else {
                    continue;
                };
                if key.len() == 8 && record.len() == ha_domain::canonical::AUTOMATION_RECORD_LEN {
                    let id = u64::from_le_bytes(key.try_into().expect("automation key"));
                    if let Err(error) = engine.add_rule(id, record) {
                        log::warn!("automation: rule {id:016x} skipped: {error:?}");
                    }
                }
            }
        }
        Err(error) => log::warn!("automation: snapshot failed: {error:?}"),
    }
    engine.build_index();
    log::info!(
        "automation: rules={} index={}",
        engine.rule_count(),
        engine.index_count()
    );
}

fn current_state(domain: &DomainHandle, entity: u64, property: u32) -> Option<f64> {
    <State<'_> as crate::StateSource>::state(&State(domain), entity, property)
}

fn emit(domain: &DomainHandle, command: ha_domain::canonical::CanonicalCommand) {
    let bytes = command.encode();
    let meta = ha_domain::FactMeta {
        source: SOURCE_AUTOMATION as u8,
        value: ha_domain::Value::enum_(command.action),
    };
    match domain.post(COMMAND_PROPERTY, &bytes, None, meta) {
        Ok(()) => log::info!(
            "automation: command posted entity={:016x} property={} action={}",
            command.entity,
            command.property,
            command.action
        ),
        Err(error) => log::warn!(
            "automation: command rejected entity={:016x} property={}: {error:?}",
            command.entity,
            command.property
        ),
    }
}

fn system_minute(domain: &DomainHandle) -> Option<(u16, u8)> {
    let minutes = current_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_MINUTES_OF_DAY)?;
    let weekday_mask = current_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_WEEKDAY_MASK)?;
    Some((minutes as u16, weekday_mask as u8))
}

fn handle_fact(domain: &DomainHandle, engine: &mut crate::RuleEngine, fact: Fact) {
    match fact.kind {
        FactKind::EntityUpserted | FactKind::EntityRemoved if fact.entity == ENTITY_AUTOMATION => {
            reload(domain, engine);
        }
        FactKind::EntityUpserted if fact.entity == ENTITY_PROPERTY_STATE => {
            let key = fact.key();
            if key.len() != STATE_KEY_LEN {
                return;
            }
            let entity = u64::from_le_bytes(key[..8].try_into().expect("state entity"));
            if entity == 0 {
                return;
            }
            let property = u32::from_le_bytes(key[8..12].try_into().expect("state property"));
            let Some(value) = current_state(domain, entity, property) else {
                return;
            };
            engine.on_state(entity, property, value, &State(domain), &mut |command| {
                emit(domain, command)
            });
        }
        FactKind::Event if fact.entity == ENTITY_ENTITY => {
            let key = fact.key();
            if key.len() != 8 {
                return;
            }
            let entity = u64::from_le_bytes(key.try_into().expect("event entity"));
            if entity == 0 {
                return;
            }
            let Some(event) = fact.value.as_u32() else {
                return;
            };
            if event == EVENT_MINUTE_TICK {
                if let Some((minutes, weekday_mask)) = system_minute(domain) {
                    engine.on_minute(
                        entity,
                        minutes,
                        weekday_mask,
                        &State(domain),
                        &mut |command| emit(domain, command),
                    );
                }
            } else {
                engine.on_event(entity, event, &State(domain), &mut |command| {
                    emit(domain, command)
                });
            }
        }
        _ => {}
    }
}

fn run(domain: DomainHandle, facts: Receiver<Fact>) {
    let mut engine = crate::RuleEngine::boxed();
    reload(&domain, &mut engine);
    while let Ok(fact) = facts.recv() {
        handle_fact(&domain, &mut engine, fact);
    }
}

/// Start the Automation service: one bounded inbox, three V2 subscriptions,
/// reload from the canonical rules and one worker thread.
pub fn start(domain: DomainHandle) -> Result<(), String> {
    let (sender, receiver) = sync_channel(INBOX_CAPACITY);
    for filter in [
        subscription(FactKind::Event.bit(), ENTITY_ENTITY),
        subscription(FactKind::EntityUpserted.bit(), ENTITY_PROPERTY_STATE),
        subscription(
            FactKind::EntityUpserted.bit() | FactKind::EntityRemoved.bit(),
            ENTITY_AUTOMATION,
        ),
    ] {
        domain
            .subscribe_channel(filter, sender.clone())
            .map_err(|error| format!("automation subscribe: {error:?}"))?;
    }
    drop(sender);

    std::thread::Builder::new()
        .name("automation".into())
        .stack_size(TASK_STACK)
        .spawn(move || run(domain, receiver))
        .map_err(|error| format!("automation task: {error:?}"))?;
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    use ha_domain::canonical::{
        CanonicalCommand, RuleAction, RuleCommandValue, RuleCompare, RuleCondition, RuleRecord,
        RuleRef, RuleTrigger, StateRecord, ACTION_ON, ENTITY_ID_SOURCE, PROPERTY_POWER,
        RULE_RECORD_LEN, SOURCE_SYSTEM, STATE_KEY_LEN,
    };
    use ha_domain::{Backing, Domain, EntityDesc, FactMeta, FactTarget, Filter, Value};

    const SENSOR: u64 = 50;
    const TARGET: u64 = 100;
    const POWER: u32 = PROPERTY_POWER;

    fn setup() -> (DomainHandle, Arc<Mutex<Vec<Vec<u8>>>>, Receiver<Fact>) {
        let mut domain = Domain::new(64, 8, 64).unwrap();
        for desc in [
            EntityDesc {
                entity: ENTITY_ENTITY,
                key_size: 8,
                record_size: 8,
                capacity: 8,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_PROPERTY_STATE,
                key_size: STATE_KEY_LEN,
                record_size: 8,
                capacity: 64,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_AUTOMATION,
                key_size: 8,
                record_size: ha_domain::canonical::AUTOMATION_RECORD_LEN,
                capacity: 16,
                backing: Backing::Ram,
                persist_key: None,
            },
        ] {
            domain.register_entity(desc).unwrap();
        }
        let handle = DomainHandle::new(domain);
        let commands = Arc::new(Mutex::new(Vec::new()));
        let captured = Arc::clone(&commands);
        handle
            .register_command(COMMAND_PROPERTY, move |args| {
                captured.lock().unwrap().push(args.to_vec());
                Ok(())
            })
            .unwrap();
        let (fact_tx, fact_rx) = std::sync::mpsc::sync_channel(8);
        handle
            .subscribe_channel(Filter::kinds(FactKind::CommandSent.bit()), fact_tx)
            .unwrap();
        (handle, commands, fact_rx)
    }

    fn rule(trigger: RuleTrigger, action: RuleAction) -> [u8; RULE_RECORD_LEN] {
        RuleRecord {
            triggers: [trigger; 4],
            actions: [action; 4],
            conditions: [RuleCondition {
                reference: RuleRef {
                    entity: 0,
                    property: 0,
                },
                compare: RuleCompare {
                    op: 0,
                    value: 0.0,
                    value2: 0.0,
                },
            }; 4],
            enabled: 1,
            trigger_count: 1,
            action_count: 1,
            conditions_count: 0,
            name: [0u8; 64],
        }
        .encode()
        .unwrap()
    }

    fn time_rule() -> [u8; RULE_RECORD_LEN] {
        rule(
            RuleTrigger {
                entity: 0,
                event_id: 0,
                property: 0,
                value: 0.0,
                value2: 0.0,
                minutes_of_day: 420,
                kind: 1,
                op: 0,
                edge: 0,
                weekday_mask: 1,
            },
            RuleAction {
                reference: RuleRef {
                    entity: ENTITY_ID_SOURCE,
                    property: POWER,
                },
                value: RuleCommandValue::None,
                action: ACTION_ON as u8,
            },
        )
    }

    fn state_rule() -> [u8; RULE_RECORD_LEN] {
        rule(
            RuleTrigger {
                entity: SENSOR,
                event_id: 0,
                property: POWER,
                value: 1.0,
                value2: 0.0,
                minutes_of_day: 0,
                kind: 2,
                op: 1,
                edge: 1,
                weekday_mask: 0,
            },
            RuleAction {
                reference: RuleRef {
                    entity: TARGET,
                    property: POWER,
                },
                value: RuleCommandValue::Scalar(Value::u32(70)),
                action: ACTION_ON as u8,
            },
        )
    }

    fn put_rule(handle: &DomainHandle, id: u64, record: &[u8; RULE_RECORD_LEN]) {
        handle
            .entity_put(
                ENTITY_AUTOMATION,
                &id.to_le_bytes(),
                record,
                FactMeta {
                    source: SOURCE_SYSTEM as u8,
                    value: Value::NONE,
                },
            )
            .unwrap();
    }

    fn put_state_with_meta(
        handle: &DomainHandle,
        entity: u64,
        property: u32,
        value: Value,
        meta_value: Value,
    ) {
        let key = StateKey { entity, property }.to_bytes();
        let record = StateRecord { value }.encode();
        handle
            .entity_put(
                ENTITY_PROPERTY_STATE,
                &key,
                &record,
                FactMeta {
                    source: SOURCE_SYSTEM as u8,
                    value: meta_value,
                },
            )
            .unwrap();
    }

    fn put_state(handle: &DomainHandle, entity: u64, property: u32, value: Value) {
        put_state_with_meta(handle, entity, property, value, value);
    }

    fn wait_commands(
        handle: &DomainHandle,
        commands: &Arc<Mutex<Vec<Vec<u8>>>>,
        count: usize,
    ) -> Vec<Vec<u8>> {
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            handle.dispatch_once().unwrap();
            let captured = commands.lock().unwrap().clone();
            if captured.len() >= count {
                return captured;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        panic!("automation command was not posted");
    }

    #[test]
    fn inbox_and_subscriptions_match_v2_contract() {
        assert_eq!(INBOX_CAPACITY, 32);
        let event = subscription(FactKind::Event.bit(), ENTITY_ENTITY);
        assert_eq!(event.kind_mask, FactKind::Event.bit());
        assert_eq!(event.entity, ENTITY_ENTITY);
        assert_eq!(event.source_mask, 0);
        let state = subscription(FactKind::EntityUpserted.bit(), ENTITY_PROPERTY_STATE);
        assert_eq!(state.kind_mask, FactKind::EntityUpserted.bit());
        assert_eq!(state.entity, ENTITY_PROPERTY_STATE);
        let automation = subscription(
            FactKind::EntityUpserted.bit() | FactKind::EntityRemoved.bit(),
            ENTITY_AUTOMATION,
        );
        assert_eq!(
            automation.kind_mask,
            FactKind::EntityUpserted.bit() | FactKind::EntityRemoved.bit()
        );
        assert_eq!(automation.entity, ENTITY_AUTOMATION);
    }
    #[test]
    fn state_uses_current_domain_value_and_enum_metadata() {
        let (handle, commands, fact_rx) = setup();
        put_rule(&handle, 1, &state_rule());
        start(handle.clone()).unwrap();

        // The fact carries a stale value; Automation must read the current record.
        put_state_with_meta(
            &handle,
            SENSOR,
            POWER,
            Value::bool(true),
            Value::bool(false),
        );
        let command_bytes = wait_commands(&handle, &commands, 1).remove(0);
        let command = CanonicalCommand::decode(&command_bytes).unwrap();
        assert_eq!(command.entity, TARGET);
        assert_eq!(command.action, ACTION_ON);

        handle.dispatch_once().unwrap();
        let fact = fact_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        assert_eq!(fact.kind, FactKind::CommandSent);
        assert_eq!(fact.value, Value::enum_(ACTION_ON));
    }

    #[test]
    fn time_uses_minute_source_and_weekday_mask() {
        let (handle, commands, _) = setup();
        put_rule(&handle, 1, &time_rule());
        start(handle.clone()).unwrap();

        put_state(
            &handle,
            ENTITY_SYSTEM,
            PROPERTY_SYSTEM_MINUTES_OF_DAY,
            Value::u32(420),
        );
        put_state(
            &handle,
            ENTITY_SYSTEM,
            PROPERTY_SYSTEM_WEEKDAY_MASK,
            Value::u32(1),
        );
        let key = ENTITY_SYSTEM.to_le_bytes();
        handle
            .payload_put(
                Some(FactTarget {
                    entity: ENTITY_ENTITY,
                    key: &key,
                }),
                FactMeta {
                    source: SOURCE_SYSTEM as u8,
                    value: Value::u32(EVENT_MINUTE_TICK),
                },
                &EVENT_MINUTE_TICK.to_le_bytes(),
            )
            .unwrap();

        let command_bytes = wait_commands(&handle, &commands, 1).remove(0);
        let command = CanonicalCommand::decode(&command_bytes).unwrap();
        assert_eq!(command.entity, ENTITY_SYSTEM);
        assert_eq!(command.action, ACTION_ON);
    }

    #[test]
    fn reload_add_and_remove_changes_rule_set() {
        let (handle, commands, _) = setup();
        start(handle.clone()).unwrap();

        put_rule(&handle, 1, &state_rule());
        put_state(&handle, SENSOR, POWER, Value::bool(true));
        assert_eq!(wait_commands(&handle, &commands, 1).len(), 1);

        handle
            .entity_remove(
                ENTITY_AUTOMATION,
                &1u64.to_le_bytes(),
                FactMeta {
                    source: SOURCE_SYSTEM as u8,
                    value: Value::NONE,
                },
            )
            .unwrap();
        handle.dispatch_once().unwrap();
        std::thread::sleep(Duration::from_millis(50));
        put_state(&handle, SENSOR, POWER, Value::bool(false));
        handle.dispatch_once().unwrap();
        std::thread::sleep(Duration::from_millis(100));
        assert_eq!(commands.lock().unwrap().len(), 1);
    }
}
