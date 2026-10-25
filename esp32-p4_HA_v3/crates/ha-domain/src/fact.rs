use micro_db::Error;

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

/// Compact value carried by a fact; numbering matches V2 `domain_value_type_t`.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ValueType {
    None = 0,
    Bool = 1,
    I32 = 2,
    U32 = 3,
    F32 = 4,
    Enum = 5,
}

/// One vocabulary for facts; the payload is the same 8 bytes as on the wire.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Value {
    pub kind: ValueType,
    bits: u32,
}

impl Value {
    pub const NONE: Self = Self {
        kind: ValueType::None,
        bits: 0,
    };

    pub const fn bool(v: bool) -> Self {
        Self {
            kind: ValueType::Bool,
            bits: v as u32,
        }
    }

    pub const fn i32(v: i32) -> Self {
        Self {
            kind: ValueType::I32,
            bits: v as u32,
        }
    }

    pub const fn u32(v: u32) -> Self {
        Self {
            kind: ValueType::U32,
            bits: v,
        }
    }

    pub fn f32(v: f32) -> Self {
        Self {
            kind: ValueType::F32,
            bits: v.to_bits(),
        }
    }

    pub const fn enum_(v: u32) -> Self {
        Self {
            kind: ValueType::Enum,
            bits: v,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        (self.kind == ValueType::Bool).then_some(self.bits != 0)
    }

    pub fn as_i32(&self) -> Option<i32> {
        (self.kind == ValueType::I32).then_some(self.bits as i32)
    }

    pub fn as_u32(&self) -> Option<u32> {
        matches!(self.kind, ValueType::U32 | ValueType::Enum).then_some(self.bits)
    }

    pub fn as_f32(&self) -> Option<f32> {
        (self.kind == ValueType::F32).then_some(f32::from_bits(self.bits))
    }

    /// V2 `domain_value_t`: type u8, three reserved bytes, little-endian u32.
    pub fn to_bytes(&self) -> [u8; 8] {
        let mut out = [0u8; 8];
        out[0] = self.kind as u8;
        out[4..].copy_from_slice(&self.bits.to_le_bytes());
        out
    }

    pub fn from_bytes(bytes: &[u8; 8]) -> Option<Self> {
        let kind = match bytes[0] {
            0 => ValueType::None,
            1 => ValueType::Bool,
            2 => ValueType::I32,
            3 => ValueType::U32,
            4 => ValueType::F32,
            5 => ValueType::Enum,
            _ => return None,
        };
        let bits = u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);
        Some(Self { kind, bits })
    }
    pub fn as_enum(&self) -> Option<u32> {
        (self.kind == ValueType::Enum).then_some(self.bits)
    }
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
