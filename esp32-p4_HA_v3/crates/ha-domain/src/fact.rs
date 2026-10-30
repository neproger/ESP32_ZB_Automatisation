use micro_db::Error;

pub use ha_model::{Value, ValueType};

pub type EntityType = u32;
pub type EventId = u64;
pub type PayloadRef = u64;

/// The fact carries the key inline; V2 has the same bound on entity keys.
pub const FACT_KEY_MAX: usize = 24;

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FactKind {
    EntityUpserted = 1,
    EntityRemoved = 2,
    Event = 3,
    CommandSent = 4,
    Error = 5,
}

impl FactKind {
    /// Bit used by subscription masks (kind 0 would mean "any").
    pub fn bit(self) -> u32 {
        1u32 << (self as u8 - 1)
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Op {
    EntityPut = 1,
    EntityRemove = 2,
    Command = 3,
    PayloadPut = 4,
    EntityGet = 5,
    EntityIter = 6,
}

/// One journal record; the event id is the ring sequence, so no lookup table.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fact {
    pub event_id: EventId,
    pub ts: u64,
    pub kind: FactKind,
    pub op: Op,
    pub source: u8,
    pub entity: EntityType,
    key: [u8; FACT_KEY_MAX],
    key_len: u8,
    pub value: Value,
    pub payload_ref: PayloadRef,
    pub payload_size: u32,
    pub error: Option<Error>,
}

impl Fact {
    /// Caller-provided key, truncated to [`FACT_KEY_MAX`] at registration.
    pub fn key(&self) -> &[u8] {
        &self.key[..self.key_len as usize]
    }

    pub(crate) fn new(
        event_id: EventId,
        ts: u64,
        kind: FactKind,
        op: Op,
        source: u8,
        entity: EntityType,
        key: &[u8],
        value: Value,
        payload_ref: PayloadRef,
        payload_size: u32,
        error: Option<Error>,
    ) -> Self {
        let key_len = key.len().min(FACT_KEY_MAX);
        let mut stored = [0u8; FACT_KEY_MAX];
        stored[..key_len].copy_from_slice(&key[..key_len]);
        Self {
            event_id,
            ts,
            kind,
            op,
            source,
            entity,
            key: stored,
            key_len: key_len as u8,
            value,
            payload_ref,
            payload_size,
            error,
        }
    }
}

/// Caller's contribution to a fact. Domain adds ts / event id / entity / key / op.
#[derive(Clone, Copy, Debug)]
pub struct FactMeta {
    pub source: u8,
    pub value: Value,
}

#[derive(Clone, Copy, Debug)]
pub struct FactTarget<'a> {
    pub entity: EntityType,
    pub key: &'a [u8],
}

/// Fact filter: zero mask / entity 0 means "any".
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Filter {
    pub kind_mask: u32,
    pub source_mask: u32,
    pub entity: EntityType,
}

impl Filter {
    pub const fn all() -> Self {
        Self {
            kind_mask: 0,
            source_mask: 0,
            entity: 0,
        }
    }

    pub const fn entity(entity: EntityType) -> Self {
        Self {
            kind_mask: 0,
            source_mask: 0,
            entity,
        }
    }

    pub const fn kinds(mask: u32) -> Self {
        Self {
            kind_mask: mask,
            source_mask: 0,
            entity: 0,
        }
    }

    pub fn matches(&self, fact: &Fact) -> bool {
        (self.kind_mask == 0 || self.kind_mask & fact.kind.bit() != 0)
            && (self.source_mask == 0
                || (fact.source < 32 && self.source_mask & (1 << fact.source) != 0))
            && (self.entity == 0 || self.entity == fact.entity)
    }
}
