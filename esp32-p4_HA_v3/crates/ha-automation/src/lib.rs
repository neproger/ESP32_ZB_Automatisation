//! Rule engine ported from V2 `automation` (rule semantics + event/state/time
//! triggers). It does not know transports: it turns canonical state/events into
//! canonical commands, and the HA layer submits them.
//!
//! Rule layout is the V2 `ha_rule_t` POD (456 bytes, little-endian, explicit
//! offsets from `ha_model/ha_rule.h`).

mod task;

pub use task::start;

use ha_domain::canonical::{
    AbiResult, CanonicalCommand, CommandValue, EntityId, PropertyId, RuleCommandValue, RuleRecord,
    ENTITY_ID_SOURCE, RULE_ACTIONS_OFFSET, RULE_ACTION_COUNT_OFFSET, RULE_ACTION_LEN,
    RULE_CONDITIONS_COUNT_OFFSET, RULE_CONDITIONS_OFFSET, RULE_CONDITION_LEN, RULE_RECORD_LEN,
    RULE_TRIGGERS_OFFSET, RULE_TRIGGER_COUNT_OFFSET, RULE_TRIGGER_LEN,
};
use ha_domain::Value;

pub const MAX_INDEX: usize = 256;
const CACHE_MAX: usize = 64;
const EPS: f64 = 1e-6;

const TRIGGER_EVENT: u8 = 0;
const TRIGGER_TIME: u8 = 1;
const TRIGGER_STATE: u8 = 2;

const EDGE_RISING: u8 = 1;
const EDGE_FALLING: u8 = 2;

/// Current values of canonical properties, used by conditions.
pub trait StateSource {
    fn state(&self, entity: EntityId, property: PropertyId) -> Option<f64>;
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Trigger {
    entity: u64,
    event_id: u32,
    property: u32,
    value: f32,
    value2: f32,
    minutes: u16,
    kind: u8,
    op: u8,
    edge: u8,
    weekday: u8,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Condition {
    entity: u64,
    property: u32,
    op: u8,
    value: f32,
    value2: f32,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Action {
    entity: u64,
    property: u32,
    action: u32,
    value: CommandValue,
}

#[derive(Clone, Copy)]
struct Rule {
    enabled: bool,
    triggers: [Option<Trigger>; 4],
    actions: [Option<Action>; 4],
    conditions: [Option<Condition>; 4],
    trigger_count: usize,
    action_count: usize,
    condition_count: usize,
}

#[derive(Clone, Copy)]
struct IndexEntry {
    rule: u16,
    trigger: u8,
    kind: u8,
    entity: u64,
    property: u32,
    event_id: u32,
}

impl IndexEntry {
    const EMPTY: Self = Self {
        rule: 0,
        trigger: 0,
        kind: 0,
        entity: 0,
        property: 0,
        event_id: 0,
    };
}

#[derive(Clone, Copy)]
struct Cached {
    entity: u64,
    property: u32,
    value: f64,
}

pub struct RuleEngine {
    rules: Vec<Rule>,
    index: [IndexEntry; MAX_INDEX],
    index_count: usize,
    cache: [Option<Cached>; CACHE_MAX],
}

impl Default for RuleEngine {
    fn default() -> Self {
        Self::new()
    }
}

impl RuleEngine {
    pub fn new() -> Self {
        Self {
            rules: Vec::new(),
            index: [IndexEntry::EMPTY; MAX_INDEX],
            index_count: 0,
            cache: [None; CACHE_MAX],
        }
    }

    /// Heap-allocated engine: the decoded rules/index are too large for the
    /// ESP-IDF main-task stack.
    pub fn boxed() -> Box<Self> {
        let mut boxed = Box::<Self>::new_uninit();
        let ptr = boxed.as_mut_ptr();
        // SAFETY: every field is initialized exactly once before `assume_init`.
        unsafe {
            std::ptr::addr_of_mut!((*ptr).rules).write(Vec::new());
            std::ptr::addr_of_mut!((*ptr).index).write([IndexEntry::EMPTY; MAX_INDEX]);
            std::ptr::addr_of_mut!((*ptr).index_count).write(0);
            std::ptr::addr_of_mut!((*ptr).cache).write([None; CACHE_MAX]);
            boxed.assume_init()
        }
    }

    pub fn clear(&mut self) {
        self.rules.clear();
        self.index_count = 0;
        self.cache = [None; CACHE_MAX];
    }

    /// Adds a decoded rule; call `build_index` after the last add.
    pub fn add_rule(&mut self, id: u64, record: &[u8]) -> AbiResult<()> {
        self.rules.push(Rule::decode(id, record)?);
        Ok(())
    }

    pub fn build_index(&mut self) {
        self.index_count = 0;
        for (slot, rule) in self.rules.iter().enumerate() {
            if !rule.enabled {
                continue;
            }
            for (index, trigger) in rule.triggers[..rule.trigger_count].iter().enumerate() {
                let Some(trigger) = trigger else {
                    continue;
                };
                if self.index_count == MAX_INDEX {
                    log::warn!("automation: index full ({MAX_INDEX}); extra triggers not indexed");
                    return;
                }
                self.index[self.index_count] = IndexEntry {
                    rule: slot as u16,
                    trigger: index as u8,
                    kind: trigger.kind,
                    entity: trigger.entity,
                    property: trigger.property,
                    event_id: trigger.event_id,
                };
                self.index_count += 1;
            }
        }
    }

    pub fn rule_count(&self) -> usize {
        self.rules.len()
    }

    pub fn index_count(&self) -> usize {
        self.index_count
    }

    pub fn on_event(
        &self,
        source: EntityId,
        event: u32,
        state: &dyn StateSource,
        emit: &mut dyn FnMut(CanonicalCommand),
    ) {
        if source == 0 {
            return;
        }
        for entry in self.index[..self.index_count].iter().copied() {
            if entry.kind != TRIGGER_EVENT || entry.event_id != event {
                continue;
            }
            if entry.entity != ENTITY_ID_SOURCE && entry.entity != source {
                continue;
            }
            self.fire(&entry, source, state, emit);
        }
    }

    pub fn on_state(
        &mut self,
        entity: EntityId,
        property: PropertyId,
        value: f64,
        state: &dyn StateSource,
        emit: &mut dyn FnMut(CanonicalCommand),
    ) {
        if entity == 0 {
            return;
        }
        let (known, previous) = self.cache_previous(entity, property);
        self.cache_put(entity, property, value);
        for entry in self.index[..self.index_count].iter().copied() {
            if entry.kind != TRIGGER_STATE || entry.property != property {
                continue;
            }
            if entry.entity != ENTITY_ID_SOURCE && entry.entity != entity {
                continue;
            }
            let Some(rule) = self.rules.get(usize::from(entry.rule)) else {
                continue;
            };
            let Some(trigger) = rule.triggers[usize::from(entry.trigger)] else {
                continue;
            };
            if state_matches(&trigger, entity, property, value, known, previous) {
                self.fire(&entry, entity, state, emit);
            }
        }
    }

    /// Minute tick from the time service (V2 `HA_EVENT_MINUTE_TICK` semantics).
    /// `source` is the entity that emitted the minute event, usually System.
    pub fn on_minute(
        &self,
        source: EntityId,
        minutes: u16,
        weekday_mask: u8,
        state: &dyn StateSource,
        emit: &mut dyn FnMut(CanonicalCommand),
    ) {
        for entry in self.index[..self.index_count].iter().copied() {
            if entry.kind != TRIGGER_TIME {
                continue;
            }
            let Some(rule) = self.rules.get(usize::from(entry.rule)) else {
                continue;
            };
            let Some(trigger) = rule.triggers[usize::from(entry.trigger)] else {
                continue;
            };
            if trigger.minutes == minutes && (trigger.weekday & weekday_mask) != 0 {
                self.fire(&entry, source, state, emit);
            }
        }
    }

    fn fire(
        &self,
        entry: &IndexEntry,
        source: EntityId,
        state: &dyn StateSource,
        emit: &mut dyn FnMut(CanonicalCommand),
    ) {
        let Some(rule) = self.rules.get(usize::from(entry.rule)) else {
            return;
        };
        if !conditions_pass(rule, source, state) {
            return;
        }
        let Some(trigger) = rule.triggers[usize::from(entry.trigger)] else {
            return;
        };
        for action in rule.actions[..rule.action_count].iter().flatten() {
            let target = resolve(action.entity, source);
            if target == 0 || target == ENTITY_ID_SOURCE {
                continue;
            }
            if action.property == 0 || action.action == 0 {
                continue;
            }
            // Loop guard: a state rule that writes the trigger's own property
            // would immediately trigger itself again.
            if trigger.kind == TRIGGER_STATE {
                let trigger_target = resolve(trigger.entity, source);
                if target == trigger_target && action.property == trigger.property {
                    continue;
                }
            }
            emit(CanonicalCommand {
                entity: target,
                property: action.property,
                action: action.action,
                value: action.value,
            });
        }
    }
    fn cache_previous(&self, entity: EntityId, property: PropertyId) -> (bool, f64) {
        for slot in self.cache.iter().flatten() {
            if slot.entity == entity && slot.property == property {
                return (true, slot.value);
            }
        }
        (false, 0.0)
    }

    fn cache_put(&mut self, entity: EntityId, property: PropertyId, value: f64) {
        for slot in &mut self.cache {
            match slot {
                Some(cached) if cached.entity == entity && cached.property == property => {
                    cached.value = value;
                    return;
                }
                None => {
                    *slot = Some(Cached {
                        entity,
                        property,
                        value,
                    });
                    return;
                }
                _ => {}
            }
        }
        // Bounded like V2: overwrite the first slot when full.
        self.cache[0] = Some(Cached {
            entity,
            property,
            value,
        });
    }
}

impl Rule {
    fn decode(_id: u64, record: &[u8]) -> AbiResult<Self> {
        let record = decode_rule(record)?;
        Ok(Self::from_record(&record))
    }

    fn from_record(record: &RuleRecord) -> Self {
        let trigger_count = usize::from(record.trigger_count);
        let action_count = usize::from(record.action_count);
        let condition_count = usize::from(record.conditions_count);
        let mut triggers = [None; 4];
        for (index, slot) in triggers.iter_mut().take(trigger_count).enumerate() {
            let trigger = record.triggers[index];
            *slot = Some(Trigger {
                entity: trigger.entity,
                event_id: trigger.event_id,
                property: trigger.property,
                value: trigger.value,
                value2: trigger.value2,
                minutes: trigger.minutes_of_day,
                kind: trigger.kind,
                op: trigger.op,
                edge: trigger.edge,
                weekday: trigger.weekday_mask,
            });
        }
        let mut actions = [None; 4];
        for (index, slot) in actions.iter_mut().take(action_count).enumerate() {
            let action = record.actions[index];
            *slot = Some(Action {
                entity: action.reference.entity,
                property: action.reference.property,
                action: u32::from(action.action),
                value: command_value(action.value),
            });
        }
        let mut conditions = [None; 4];
        for (index, slot) in conditions.iter_mut().take(condition_count).enumerate() {
            let condition = record.conditions[index];
            *slot = Some(Condition {
                entity: condition.reference.entity,
                property: condition.reference.property,
                op: condition.compare.op,
                value: condition.compare.value,
                value2: condition.compare.value2,
            });
        }
        Self {
            enabled: record.enabled != 0,
            triggers,
            actions,
            conditions,
            trigger_count,
            action_count,
            condition_count,
        }
    }
}

/// Decode a canonical rule with V2 normalization.
///
/// V2 clamps the three counters to the array limit and ignores inactive slots.
/// Active entries still go through the strict canonical decoder; a malformed
/// active entry rejects only this rule.
pub fn decode_rule(record: &[u8]) -> AbiResult<RuleRecord> {
    if record.len() != RULE_RECORD_LEN {
        return Err(ha_domain::canonical::AbiError::InvalidSize);
    }

    let mut normalized = [0u8; RULE_RECORD_LEN];
    normalized.copy_from_slice(record);

    let trigger_count = normalized[RULE_TRIGGER_COUNT_OFFSET].min(4);
    let action_count = normalized[RULE_ACTION_COUNT_OFFSET].min(4);
    let condition_count = normalized[RULE_CONDITIONS_COUNT_OFFSET].min(4);
    normalized[RULE_TRIGGER_COUNT_OFFSET] = trigger_count;
    normalized[RULE_ACTION_COUNT_OFFSET] = action_count;
    normalized[RULE_CONDITIONS_COUNT_OFFSET] = condition_count;

    for index in usize::from(trigger_count)..4 {
        let start = RULE_TRIGGERS_OFFSET + index * RULE_TRIGGER_LEN;
        normalized[start..start + RULE_TRIGGER_LEN].fill(0);
    }
    for index in usize::from(action_count)..4 {
        let start = RULE_ACTIONS_OFFSET + index * RULE_ACTION_LEN;
        normalized[start..start + RULE_ACTION_LEN].fill(0);
    }
    for index in usize::from(condition_count)..4 {
        let start = RULE_CONDITIONS_OFFSET + index * RULE_CONDITION_LEN;
        normalized[start..start + RULE_CONDITION_LEN].fill(0);
    }

    RuleRecord::decode(&normalized)
}

/// V2 `SetAutomationEnabled`: decode the canonical rule, change only `enabled`,
/// then encode it again. The same V2 normalization/malformed policy applies.
pub fn set_enabled(record: &[u8], enabled: bool) -> AbiResult<[u8; RULE_RECORD_LEN]> {
    let mut rule = decode_rule(record)?;
    rule.enabled = u8::from(enabled);
    rule.encode()
}
fn conditions_pass(rule: &Rule, source: EntityId, state: &dyn StateSource) -> bool {
    for condition in rule.conditions[..rule.condition_count].iter().flatten() {
        let entity = resolve(condition.entity, source);
        if entity == 0 {
            return false;
        }
        let Some(actual) = state.state(entity, condition.property) else {
            return false;
        };
        if !compare(
            condition.op,
            f64::from(condition.value),
            f64::from(condition.value2),
            actual,
        ) {
            return false;
        }
    }
    true
}

fn state_matches(
    trigger: &Trigger,
    entity: EntityId,
    property: PropertyId,
    value: f64,
    known: bool,
    previous: f64,
) -> bool {
    if trigger.kind != TRIGGER_STATE {
        return false;
    }
    if trigger.entity != ENTITY_ID_SOURCE && trigger.entity != entity {
        return false;
    }
    if trigger.property != property {
        return false;
    }
    let lo = f64::from(trigger.value);
    let hi = f64::from(trigger.value2);
    let holds = compare(trigger.op, lo, hi, value);
    let previous_holds = compare(trigger.op, lo, hi, previous);
    match trigger.edge {
        EDGE_RISING => holds && (!known || !previous_holds),
        EDGE_FALLING => !holds && known && previous_holds,
        _ => holds,
    }
}

fn compare(op: u8, lo: f64, hi: f64, actual: f64) -> bool {
    match op {
        1 => (actual - lo).abs() <= EPS,
        2 => (actual - lo).abs() > EPS,
        3 => actual > lo,
        4 => actual < lo,
        5 => actual >= lo,
        6 => actual <= lo,
        7 => ((actual as u32) & (lo as u32)) != 0,
        8 => actual >= lo && actual <= hi,
        _ => false,
    }
}

fn resolve(reference: EntityId, source: EntityId) -> EntityId {
    if reference == ENTITY_ID_SOURCE {
        source
    } else {
        reference
    }
}

fn command_value(value: RuleCommandValue) -> CommandValue {
    match value {
        RuleCommandValue::None => CommandValue::None,
        RuleCommandValue::Scalar(value) => CommandValue::Scalar(value),
        RuleCommandValue::Xy { x, y } => CommandValue::Xy { x, y },
    }
}

pub fn value_to_f64(value: &Value) -> Option<f64> {
    if let Some(v) = value.as_bool() {
        return Some(if v { 1.0 } else { 0.0 });
    }
    if let Some(v) = value.as_f32() {
        return Some(f64::from(v));
    }
    if let Some(v) = value.as_i32() {
        return Some(f64::from(v));
    }
    value.as_u32().map(f64::from)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::canonical::{
        AbiError, RuleAction, RuleCommandValue, RuleCompare, RuleCondition, RuleRef, RuleTrigger,
        ACTION_ON, PROPERTY_BRIGHTNESS, PROPERTY_POWER, RULE_ACTION_RESERVED_OFFSET,
        RULE_ACTION_VALUE_OFFSET, RULE_COMMAND_KIND_OFFSET, RULE_CONDITION_RESERVED_OFFSET,
        RULE_ENABLED_OFFSET, RULE_RESERVED_OFFSET, RULE_TRIGGER_EDGE_OFFSET,
        RULE_TRIGGER_RESERVED_OFFSET,
    };
    use std::collections::HashMap;

    fn canonical_record() -> RuleRecord {
        let trigger = RuleTrigger {
            entity: 50,
            event_id: 1,
            property: 0,
            value: 0.0,
            value2: 0.0,
            minutes_of_day: 0,
            kind: TRIGGER_EVENT,
            op: 0,
            edge: 0,
            weekday_mask: 0,
        };
        let action = RuleAction {
            reference: RuleRef {
                entity: 100,
                property: PROPERTY_POWER,
            },
            value: RuleCommandValue::Scalar(Value::u32(70)),
            action: ACTION_ON as u8,
        };
        let condition = RuleCondition {
            reference: RuleRef {
                entity: 50,
                property: PROPERTY_BRIGHTNESS,
            },
            compare: RuleCompare {
                op: 3,
                value: 40.0,
                value2: 0.0,
            },
        };
        RuleRecord {
            triggers: [trigger; 4],
            actions: [action; 4],
            conditions: [condition; 4],
            enabled: 1,
            trigger_count: 1,
            action_count: 1,
            conditions_count: 1,
            name: [0u8; 64],
        }
    }

    #[test]
    fn canonical_rule_roundtrip() {
        let mut record = canonical_record();
        record.trigger_count = 4;
        record.action_count = 4;
        record.conditions_count = 4;
        let bytes = record.encode().unwrap();
        let decoded = decode_rule(&bytes).unwrap();
        assert_eq!(decoded, record);
    }

    #[test]
    fn set_enabled_preserves_the_rule() {
        let mut record = canonical_record();
        record.trigger_count = 4;
        record.action_count = 4;
        record.conditions_count = 4;
        let bytes = record.encode().unwrap();
        let disabled = set_enabled(&bytes, false).unwrap();
        let decoded = decode_rule(&disabled).unwrap();
        assert_eq!(decoded.enabled, 0);
        assert_eq!(decoded.triggers[0], record.triggers[0]);
        assert_eq!(decoded.actions[0], record.actions[0]);
        assert_eq!(decoded.conditions[0], record.conditions[0]);
        assert_eq!(set_enabled(&bytes, true).unwrap(), bytes);
    }

    #[test]
    fn set_enabled_rejects_malformed_rule() {
        let record = canonical_record();
        let mut bytes = record.encode().unwrap();
        bytes[RULE_RESERVED_OFFSET] = 1;
        assert_eq!(set_enabled(&bytes, true), Err(AbiError::NonZeroReserved));
    }
    #[test]
    fn counts_above_max_are_clamped() {
        let record = canonical_record();
        let mut bytes = record.encode().unwrap();
        bytes[RULE_TRIGGER_COUNT_OFFSET] = 5;
        bytes[RULE_ACTION_COUNT_OFFSET] = 6;
        bytes[RULE_CONDITIONS_COUNT_OFFSET] = 7;
        let decoded = decode_rule(&bytes).unwrap();
        assert_eq!(decoded.trigger_count, 4);
        assert_eq!(decoded.action_count, 4);
        assert_eq!(decoded.conditions_count, 4);
    }

    #[test]
    fn inactive_slots_are_ignored() {
        let record = canonical_record();
        let mut bytes = record.encode().unwrap();
        for index in 1..4 {
            let trigger = RULE_TRIGGERS_OFFSET + index * RULE_TRIGGER_LEN;
            bytes[trigger..trigger + RULE_TRIGGER_LEN].fill(0xFF);
            let action = RULE_ACTIONS_OFFSET + index * RULE_ACTION_LEN;
            bytes[action..action + RULE_ACTION_LEN].fill(0xFF);
            let condition = RULE_CONDITIONS_OFFSET + index * RULE_CONDITION_LEN;
            bytes[condition..condition + RULE_CONDITION_LEN].fill(0xFF);
        }
        assert!(decode_rule(&bytes).is_ok());
    }

    #[test]
    fn active_reserved_bytes_are_malformed() {
        let record = canonical_record();
        let mut bytes = record.encode().unwrap();
        bytes[RULE_TRIGGER_RESERVED_OFFSET] = 1;
        assert_eq!(decode_rule(&bytes), Err(AbiError::NonZeroReserved));
    }

    #[test]
    fn action_and_condition_reserved_bytes_are_malformed() {
        let record = canonical_record();
        for offset in [
            RULE_ACTIONS_OFFSET + RULE_ACTION_RESERVED_OFFSET,
            RULE_CONDITIONS_OFFSET + RULE_CONDITION_RESERVED_OFFSET,
        ] {
            let mut bytes = record.encode().unwrap();
            bytes[offset] = 1;
            assert_eq!(decode_rule(&bytes), Err(AbiError::NonZeroReserved));
        }
    }

    #[test]
    fn wrong_record_length_is_malformed() {
        assert_eq!(
            decode_rule(&[0u8; RULE_RECORD_LEN - 1]),
            Err(AbiError::InvalidSize)
        );
    }

    #[test]
    fn record_reserved_bytes_are_malformed() {
        let record = canonical_record();
        let mut bytes = record.encode().unwrap();
        bytes[RULE_RESERVED_OFFSET] = 1;
        assert_eq!(decode_rule(&bytes), Err(AbiError::NonZeroReserved));
    }

    #[test]
    fn invalid_active_command_value_is_malformed() {
        let record = canonical_record();
        let mut bytes = record.encode().unwrap();
        bytes[RULE_ACTIONS_OFFSET + RULE_ACTION_VALUE_OFFSET + RULE_COMMAND_KIND_OFFSET] = 9;
        assert_eq!(decode_rule(&bytes), Err(AbiError::InvalidKind));
    }

    #[test]
    fn enabled_nonzero_and_unknown_edge_follow_v2() {
        let record = canonical_record();
        let mut bytes = record.encode().unwrap();
        bytes[RULE_ENABLED_OFFSET] = 2;
        bytes[RULE_TRIGGER_EDGE_OFFSET] = 99;
        let decoded = decode_rule(&bytes).unwrap();
        assert_eq!(decoded.enabled, 2);
        assert_eq!(decoded.triggers[0].edge, 99);
        assert!(Rule::decode(1, &bytes).unwrap().enabled);
    }

    #[derive(Default)]
    struct Values(HashMap<(u64, u32), f64>);

    impl StateSource for Values {
        fn state(&self, entity: EntityId, property: PropertyId) -> Option<f64> {
            self.0.get(&(entity, property)).copied()
        }
    }

    struct RuleBuilder {
        record: [u8; RULE_RECORD_LEN],
        triggers: usize,
        actions: usize,
        conditions: usize,
    }

    impl RuleBuilder {
        fn new() -> Self {
            let mut record = [0u8; RULE_RECORD_LEN];
            record[384] = 1;
            Self {
                record,
                triggers: 0,
                actions: 0,
                conditions: 0,
            }
        }

        fn trigger(
            mut self,
            kind: u8,
            entity: u64,
            event: u32,
            property: u32,
            op: u8,
            edge: u8,
            value: f32,
            minutes: u16,
            weekday: u8,
        ) -> Self {
            let base = self.triggers * 32;
            let r = &mut self.record;
            r[base..base + 8].copy_from_slice(&entity.to_le_bytes());
            r[base + 8..base + 12].copy_from_slice(&event.to_le_bytes());
            r[base + 12..base + 16].copy_from_slice(&property.to_le_bytes());
            r[base + 16..base + 20].copy_from_slice(&value.to_le_bytes());
            r[base + 24..base + 26].copy_from_slice(&minutes.to_le_bytes());
            r[base + 26] = kind;
            r[base + 27] = op;
            r[base + 28] = edge;
            r[base + 29] = weekday;
            self.triggers += 1;
            self.record[385] = self.triggers as u8;
            self
        }

        fn action(mut self, entity: u64, property: u32, action: u32) -> Self {
            let base = 128 + self.actions * 32;
            let r = &mut self.record;
            r[base..base + 8].copy_from_slice(&entity.to_le_bytes());
            r[base + 8..base + 12].copy_from_slice(&property.to_le_bytes());
            r[base + 28] = action as u8;
            self.actions += 1;
            self.record[386] = self.actions as u8;
            self
        }

        fn condition(mut self, entity: u64, property: u32, op: u8, value: f32) -> Self {
            let base = 256 + self.conditions * 32;
            let r = &mut self.record;
            r[base..base + 8].copy_from_slice(&entity.to_le_bytes());
            r[base + 8..base + 12].copy_from_slice(&property.to_le_bytes());
            r[base + 16] = op;
            r[base + 20..base + 24].copy_from_slice(&value.to_le_bytes());
            self.conditions += 1;
            self.record[387] = self.conditions as u8;
            self
        }

        fn build(self) -> [u8; RULE_RECORD_LEN] {
            self.record
        }
    }

    fn engine(rule: &[u8; RULE_RECORD_LEN]) -> RuleEngine {
        let mut engine = RuleEngine::new();
        engine.add_rule(1, rule).unwrap();
        engine.build_index();
        engine
    }

    #[test]
    fn event_trigger_fires_action() {
        let rule = RuleBuilder::new()
            .trigger(TRIGGER_EVENT, ENTITY_ID_SOURCE, 1, 0, 0, 0, 0.0, 0, 0)
            .action(100, PROPERTY_POWER, ACTION_ON)
            .build();
        let engine = engine(&rule);
        let mut actions = Vec::new();
        engine.on_event(50, 1, &Values::default(), &mut |command| {
            actions.push(command)
        });
        let action = actions.iter().next().copied().unwrap();
        assert_eq!(action.entity, 100);
        assert_eq!(action.action, ACTION_ON);
    }

    #[test]
    fn state_trigger_rising_uses_previous_value() {
        let rule = RuleBuilder::new()
            .trigger(
                TRIGGER_STATE,
                ENTITY_ID_SOURCE,
                0,
                PROPERTY_POWER,
                1,
                1,
                1.0,
                0,
                0,
            )
            .action(100, PROPERTY_POWER, ACTION_ON)
            .build();
        let mut engine = engine(&rule);
        let state = Values::default();
        let mut actions = Vec::new();
        engine.on_state(50, PROPERTY_POWER, 1.0, &state, &mut |command| {
            actions.push(command)
        });
        assert_eq!(actions.len(), 1);
        actions.clear();
        engine.on_state(50, PROPERTY_POWER, 1.0, &state, &mut |command| {
            actions.push(command)
        });
        assert!(actions.is_empty());
        engine.on_state(50, PROPERTY_POWER, 0.0, &state, &mut |command| {
            actions.push(command)
        });
        engine.on_state(50, PROPERTY_POWER, 1.0, &state, &mut |command| {
            actions.push(command)
        });
        assert_eq!(actions.len(), 1);
    }

    #[test]
    fn conditions_must_pass() {
        let rule = RuleBuilder::new()
            .trigger(TRIGGER_EVENT, ENTITY_ID_SOURCE, 1, 0, 0, 0, 0.0, 0, 0)
            .condition(50, PROPERTY_BRIGHTNESS, 3, 50.0)
            .action(100, PROPERTY_POWER, ACTION_ON)
            .build();
        let engine = engine(&rule);
        let mut values = Values::default();
        let mut actions = Vec::new();
        values.0.insert((50, PROPERTY_BRIGHTNESS), 40.0);
        engine.on_event(50, 1, &values, &mut |command| actions.push(command));
        assert!(actions.is_empty());
        values.0.insert((50, PROPERTY_BRIGHTNESS), 80.0);
        engine.on_event(50, 1, &values, &mut |command| actions.push(command));
        assert_eq!(actions.len(), 1);
    }

    #[test]
    fn time_trigger_matches_minute_and_weekday() {
        let rule = RuleBuilder::new()
            .trigger(TRIGGER_TIME, 0, 0, 0, 0, 0, 0.0, 420, 0b0000010)
            .action(100, PROPERTY_POWER, ACTION_ON)
            .build();
        let engine = engine(&rule);
        let mut actions = Vec::new();
        engine.on_minute(50, 420, 0b0000001, &Values::default(), &mut |command| {
            actions.push(command)
        });
        assert!(actions.is_empty());
        engine.on_minute(50, 421, 0b0000010, &Values::default(), &mut |command| {
            actions.push(command)
        });
        assert!(actions.is_empty());
        engine.on_minute(50, 420, 0b0000010, &Values::default(), &mut |command| {
            actions.push(command)
        });
        assert_eq!(actions.len(), 1);
    }

    #[test]
    fn time_trigger_source_resolves_to_minute_source() {
        let rule = RuleBuilder::new()
            .trigger(TRIGGER_TIME, 0, 0, 0, 0, 0, 0.0, 420, 0b0000001)
            .action(ENTITY_ID_SOURCE, PROPERTY_POWER, ACTION_ON)
            .build();
        let engine = engine(&rule);
        let mut actions = Vec::new();
        engine.on_minute(50, 420, 0b0000001, &Values::default(), &mut |command| {
            actions.push(command)
        });
        assert_eq!(actions.len(), 1);
        assert_eq!(actions[0].entity, 50);
    }

    #[test]
    fn multiple_conditions_are_anded() {
        let rule = RuleBuilder::new()
            .trigger(TRIGGER_EVENT, ENTITY_ID_SOURCE, 1, 0, 0, 0, 0.0, 0, 0)
            .condition(50, PROPERTY_POWER, 1, 1.0)
            .condition(50, PROPERTY_BRIGHTNESS, 3, 50.0)
            .action(100, PROPERTY_POWER, ACTION_ON)
            .build();
        let engine = engine(&rule);
        let mut values = Values::default();
        values.0.insert((50, PROPERTY_POWER), 1.0);
        values.0.insert((50, PROPERTY_BRIGHTNESS), 40.0);
        let mut actions = Vec::new();
        engine.on_event(50, 1, &values, &mut |command| actions.push(command));
        assert!(actions.is_empty());
        values.0.insert((50, PROPERTY_BRIGHTNESS), 80.0);
        engine.on_event(50, 1, &values, &mut |command| actions.push(command));
        assert_eq!(actions.len(), 1);
    }

    #[test]
    fn more_than_sixteen_rules_and_actions_are_not_dropped() {
        let mut engine = RuleEngine::new();
        for rule_index in 0..20 {
            let mut builder =
                RuleBuilder::new().trigger(TRIGGER_EVENT, ENTITY_ID_SOURCE, 1, 0, 0, 0, 0.0, 0, 0);
            for action_index in 0..4 {
                builder = builder.action(
                    1_000 + rule_index * 4 + action_index,
                    PROPERTY_POWER,
                    ACTION_ON,
                );
            }
            engine
                .add_rule(rule_index as u64, &builder.build())
                .unwrap();
        }
        engine.build_index();
        assert_eq!(engine.rule_count(), 20);
        assert_eq!(engine.index_count(), 20);
        let mut actions = Vec::new();
        engine.on_event(50, 1, &Values::default(), &mut |command| {
            actions.push(command)
        });
        assert_eq!(actions.len(), 80);
    }
    #[test]
    fn state_action_on_its_own_property_is_skipped() {
        let rule = RuleBuilder::new()
            .trigger(
                TRIGGER_STATE,
                ENTITY_ID_SOURCE,
                0,
                PROPERTY_POWER,
                1,
                0,
                1.0,
                0,
                0,
            )
            .action(ENTITY_ID_SOURCE, PROPERTY_POWER, ACTION_ON)
            .build();
        let mut engine = engine(&rule);
        let mut actions = Vec::new();
        engine.on_state(
            50,
            PROPERTY_POWER,
            1.0,
            &Values::default(),
            &mut |command| actions.push(command),
        );
        assert!(actions.is_empty());
    }
}
