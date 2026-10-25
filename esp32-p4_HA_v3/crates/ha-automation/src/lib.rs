//! Rule engine ported from V2 `automation` (rule semantics + event/state/time
//! triggers). It does not know transports: it turns canonical state/events into
//! canonical commands, and the HA layer submits them.
//!
//! Rule layout is the V2 `ha_rule_t` POD (456 bytes, little-endian, explicit
//! offsets from `ha_model/ha_rule.h`).

use ha_domain::canonical::{
    CanonicalCommand, CommandValue, EntityId, PropertyId, ENTITY_ID_SOURCE,
};
use ha_domain::Value;

pub const RULE_RECORD_LEN: usize = 456;
pub const MAX_RULES: usize = 16;
pub const MAX_INDEX: usize = 256;
pub const MAX_ACTIONS: usize = 16;
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
    rule: u8,
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

/// Bounded command batch produced by one fact.
pub struct Actions {
    items: [Option<CanonicalCommand>; MAX_ACTIONS],
    len: usize,
}

impl Default for Actions {
    fn default() -> Self {
        Self {
            items: [None; MAX_ACTIONS],
            len: 0,
        }
    }
}

impl Actions {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&mut self) {
        self.len = 0;
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn iter(&self) -> impl Iterator<Item = &CanonicalCommand> {
        self.items[..self.len].iter().flatten()
    }

    fn push(&mut self, command: CanonicalCommand) {
        if self.len < MAX_ACTIONS {
            self.items[self.len] = Some(command);
            self.len += 1;
        }
    }
}

pub struct RuleEngine {
    rules: [Option<Rule>; MAX_RULES],
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
            rules: [None; MAX_RULES],
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
        unsafe {
            std::ptr::addr_of_mut!((*ptr).rules).write([None; MAX_RULES]);
            std::ptr::addr_of_mut!((*ptr).index).write([IndexEntry::EMPTY; MAX_INDEX]);
            std::ptr::addr_of_mut!((*ptr).index_count).write(0);
            std::ptr::addr_of_mut!((*ptr).cache).write([None; CACHE_MAX]);
            boxed.assume_init()
        }
    }

    pub fn clear(&mut self) {
        self.rules = [None; MAX_RULES];
        self.index_count = 0;
        self.cache = [None; CACHE_MAX];
    }

    /// Adds a decoded rule; call `build_index` after the last add.
    pub fn add_rule(&mut self, id: u64, record: &[u8]) {
        let Some(rule) = Rule::decode(id, record) else {
            return;
        };
        if let Some(slot) = self.rules.iter().position(Option::is_none) {
            self.rules[slot] = Some(rule);
        }
    }

    pub fn build_index(&mut self) {
        self.index_count = 0;
        for (slot, rule) in self.rules.iter().enumerate() {
            let Some(rule) = rule else {
                continue;
            };
            if !rule.enabled {
                continue;
            }
            for (index, trigger) in rule.triggers[..rule.trigger_count].iter().enumerate() {
                let Some(trigger) = trigger else {
                    continue;
                };
                if self.index_count == MAX_INDEX {
                    return;
                }
                self.index[self.index_count] = IndexEntry {
                    rule: slot as u8,
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
        self.rules.iter().flatten().count()
    }

    pub fn index_count(&self) -> usize {
        self.index_count
    }

    pub fn on_event(
        &self,
        source: EntityId,
        event: u32,
        state: &dyn StateSource,
        out: &mut Actions,
    ) {
        for entry in self.index[..self.index_count].iter().copied() {
            if entry.kind != TRIGGER_EVENT || entry.event_id != event {
                continue;
            }
            if entry.entity != ENTITY_ID_SOURCE && entry.entity != source {
                continue;
            }
            self.fire(&entry, source, state, out);
        }
    }

    pub fn on_state(
        &mut self,
        entity: EntityId,
        property: PropertyId,
        value: f64,
        state: &dyn StateSource,
        out: &mut Actions,
    ) {
        let (known, previous) = self.cache_previous(entity, property);
        self.cache_put(entity, property, value);
        for entry in self.index[..self.index_count].iter().copied() {
            if entry.kind != TRIGGER_STATE || entry.property != property {
                continue;
            }
            if entry.entity != ENTITY_ID_SOURCE && entry.entity != entity {
                continue;
            }
            let Some(rule) = self.rules[usize::from(entry.rule)] else {
                continue;
            };
            let Some(trigger) = rule.triggers[usize::from(entry.trigger)] else {
                continue;
            };
            if state_matches(&trigger, entity, property, value, known, previous) {
                self.fire(&entry, entity, state, out);
            }
        }
    }

    /// Minute tick from the time service (V2 `HA_EVENT_MINUTE_TICK` semantics).
    pub fn on_minute(
        &self,
        minutes: u16,
        weekday_mask: u8,
        state: &dyn StateSource,
        out: &mut Actions,
    ) {
        for entry in self.index[..self.index_count].iter().copied() {
            if entry.kind != TRIGGER_TIME {
                continue;
            }
            let Some(rule) = self.rules[usize::from(entry.rule)] else {
                continue;
            };
            let Some(trigger) = rule.triggers[usize::from(entry.trigger)] else {
                continue;
            };
            if trigger.minutes == minutes && (trigger.weekday & weekday_mask) != 0 {
                self.fire(&entry, 0, state, out);
            }
        }
    }

    fn fire(
        &self,
        entry: &IndexEntry,
        source: EntityId,
        state: &dyn StateSource,
        out: &mut Actions,
    ) {
        let Some(rule) = self.rules[usize::from(entry.rule)] else {
            return;
        };
        if !conditions_pass(&rule, source, state) {
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
            out.push(CanonicalCommand {
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
    fn decode(_id: u64, record: &[u8]) -> Option<Self> {
        if record.len() != RULE_RECORD_LEN {
            return None;
        }
        let trigger_count = usize::from(record[385]).min(4);
        let action_count = usize::from(record[386]).min(4);
        let condition_count = usize::from(record[387]).min(4);
        let mut triggers = [None; 4];
        for (index, slot) in triggers.iter_mut().enumerate() {
            if index >= trigger_count {
                break;
            }
            let base = index * 32;
            *slot = Some(Trigger {
                entity: u64_at(record, base),
                event_id: u32_at(record, base + 8),
                property: u32_at(record, base + 12),
                value: f32_at(record, base + 16),
                value2: f32_at(record, base + 20),
                minutes: u16_at(record, base + 24),
                kind: record[base + 26],
                op: record[base + 27],
                edge: record[base + 28],
                weekday: record[base + 29],
            });
        }
        let mut actions = [None; 4];
        for (index, slot) in actions.iter_mut().enumerate() {
            if index >= action_count {
                break;
            }
            let base = 128 + index * 32;
            *slot = Some(Action {
                entity: u64_at(record, base),
                property: u32_at(record, base + 8),
                action: u32::from(record[base + 28]),
                value: command_value(&record[base + 16..base + 28]),
            });
        }
        let mut conditions = [None; 4];
        for (index, slot) in conditions.iter_mut().enumerate() {
            if index >= condition_count {
                break;
            }
            let base = 256 + index * 32;
            *slot = Some(Condition {
                entity: u64_at(record, base),
                property: u32_at(record, base + 8),
                op: record[base + 16],
                value: f32_at(record, base + 20),
                value2: f32_at(record, base + 24),
            });
        }
        Some(Self {
            enabled: record[384] != 0,
            triggers,
            actions,
            conditions,
            trigger_count,
            action_count,
            condition_count,
        })
    }
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

fn command_value(bytes: &[u8]) -> CommandValue {
    match i32_at(bytes, 0) {
        0 => CommandValue::None,
        1 => {
            let kind = i32_at(bytes, 4);
            let bits = u32_at(bytes, 8);
            let value = match kind {
                1 => Value::bool(bits != 0),
                2 => Value::i32(bits as i32),
                3 => Value::u32(bits),
                4 => Value::f32(f32::from_bits(bits)),
                5 => Value::enum_(bits),
                _ => return CommandValue::None,
            };
            CommandValue::Scalar(value)
        }
        2 => CommandValue::Xy {
            x: f32_at(bytes, 4),
            y: f32_at(bytes, 8),
        },
        _ => CommandValue::None,
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

fn u16_at(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn i32_at(bytes: &[u8], offset: usize) -> i32 {
    u32_at(bytes, offset) as i32
}

fn u64_at(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
        bytes[offset + 4],
        bytes[offset + 5],
        bytes[offset + 6],
        bytes[offset + 7],
    ])
}

fn f32_at(bytes: &[u8], offset: usize) -> f32 {
    f32::from_bits(u32_at(bytes, offset))
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::canonical::{ACTION_ON, PROPERTY_BRIGHTNESS, PROPERTY_POWER};
    use std::collections::HashMap;

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
        engine.add_rule(1, rule);
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
        let mut actions = Actions::new();
        engine.on_event(50, 1, &Values::default(), &mut actions);
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
        let mut actions = Actions::new();
        engine.on_state(50, PROPERTY_POWER, 1.0, &state, &mut actions);
        assert_eq!(actions.len(), 1);
        actions.clear();
        engine.on_state(50, PROPERTY_POWER, 1.0, &state, &mut actions);
        assert!(actions.is_empty());
        engine.on_state(50, PROPERTY_POWER, 0.0, &state, &mut actions);
        engine.on_state(50, PROPERTY_POWER, 1.0, &state, &mut actions);
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
        let mut actions = Actions::new();
        values.0.insert((50, PROPERTY_BRIGHTNESS), 40.0);
        engine.on_event(50, 1, &values, &mut actions);
        assert!(actions.is_empty());
        values.0.insert((50, PROPERTY_BRIGHTNESS), 80.0);
        engine.on_event(50, 1, &values, &mut actions);
        assert_eq!(actions.len(), 1);
    }

    #[test]
    fn time_trigger_matches_minute_and_weekday() {
        let rule = RuleBuilder::new()
            .trigger(TRIGGER_TIME, 0, 0, 0, 0, 0, 0.0, 420, 0b0000010)
            .action(100, PROPERTY_POWER, ACTION_ON)
            .build();
        let engine = engine(&rule);
        let mut actions = Actions::new();
        engine.on_minute(420, 0b0000001, &Values::default(), &mut actions);
        assert!(actions.is_empty());
        engine.on_minute(421, 0b0000010, &Values::default(), &mut actions);
        assert!(actions.is_empty());
        engine.on_minute(420, 0b0000010, &Values::default(), &mut actions);
        assert_eq!(actions.len(), 1);
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
        let mut actions = Actions::new();
        engine.on_state(50, PROPERTY_POWER, 1.0, &Values::default(), &mut actions);
        assert!(actions.is_empty());
    }
}
