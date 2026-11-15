use super::*;
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum CommandValue {
    None,
    Scalar(Value),
    Xy { x: f32, y: f32 },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CanonicalCommand {
    pub entity: EntityId,
    pub property: PropertyId,
    pub action: ActionId,
    pub value: CommandValue,
}

impl CanonicalCommand {
    /// V2 `ha_property_command_t`: entity u64@0, property u32@8, action u8@12,
    /// value kind u32@16, scalar kind / x @20, value / y @24.
    pub fn encode(self) -> [u8; COMMAND_RECORD_LEN] {
        let mut out = [0u8; COMMAND_RECORD_LEN];
        out[..8].copy_from_slice(&self.entity.to_le_bytes());
        out[8..12].copy_from_slice(&self.property.to_le_bytes());
        out[12] = self.action as u8;
        match self.value {
            CommandValue::None => {}
            CommandValue::Scalar(value) => {
                let bytes = value.to_bytes();
                out[16..20].copy_from_slice(&1u32.to_le_bytes());
                out[20..24].copy_from_slice(&u32::from(bytes[0]).to_le_bytes());
                out[24..28].copy_from_slice(&bytes[4..8]);
            }
            CommandValue::Xy { x, y } => {
                out[16..20].copy_from_slice(&2u32.to_le_bytes());
                out[20..24].copy_from_slice(&x.to_le_bytes());
                out[24..28].copy_from_slice(&y.to_le_bytes());
            }
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != COMMAND_RECORD_LEN {
            return None;
        }
        let entity = u64::from_le_bytes(bytes[..8].try_into().ok()?);
        let property = u32::from_le_bytes(bytes[8..12].try_into().ok()?);
        let action = u32::from(bytes[12]);
        let kind = u32::from_le_bytes(bytes[16..20].try_into().ok()?);
        let value = match kind {
            0 => CommandValue::None,
            1 => {
                let scalar = u32::from_le_bytes(bytes[20..24].try_into().ok()?);
                let bits = u32::from_le_bytes(bytes[24..28].try_into().ok()?);
                let value = match scalar {
                    1 => Value::bool(bits != 0),
                    2 => Value::i32(bits as i32),
                    3 => Value::u32(bits),
                    4 => Value::f32(f32::from_bits(bits)),
                    5 => Value::enum_(bits),
                    _ => return None,
                };
                CommandValue::Scalar(value)
            }
            2 => CommandValue::Xy {
                x: f32::from_le_bytes(bytes[20..24].try_into().ok()?),
                y: f32::from_le_bytes(bytes[24..28].try_into().ok()?),
            },
            _ => return None,
        };
        Some(Self {
            entity,
            property,
            action,
            value,
        })
    }
}
