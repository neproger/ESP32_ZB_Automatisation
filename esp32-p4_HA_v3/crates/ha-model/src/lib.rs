//! Canonical HA model and record ABI, independent from Domain storage.

pub mod canonical;

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
