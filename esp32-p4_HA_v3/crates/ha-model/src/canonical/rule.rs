use super::*;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RuleRef {
    pub entity: EntityId,
    pub property: PropertyId,
}

impl RuleRef {
    const EMPTY: Self = Self {
        entity: 0,
        property: 0,
    };

    pub fn encode(self) -> [u8; RULE_REF_LEN] {
        let mut out = [0u8; RULE_REF_LEN];
        abi_put_u64(&mut out, RULE_REF_ENTITY_OFFSET, self.entity);
        abi_put_u32(&mut out, RULE_REF_PROPERTY_OFFSET, self.property);
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_REF_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(bytes, RULE_REF_RESERVED_OFFSET, 4)?;
        Ok(Self {
            entity: abi_get_u64(bytes, RULE_REF_ENTITY_OFFSET),
            property: abi_get_u32(bytes, RULE_REF_PROPERTY_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleCompare {
    pub op: u8,
    pub value: f32,
    pub value2: f32,
}

impl RuleCompare {
    const EMPTY: Self = Self {
        op: 0,
        value: 0.0,
        value2: 0.0,
    };

    pub fn encode(self) -> [u8; RULE_COMPARE_LEN] {
        let mut out = [0u8; RULE_COMPARE_LEN];
        out[RULE_COMPARE_OP_OFFSET] = self.op;
        abi_put_u32(&mut out, RULE_COMPARE_VALUE_OFFSET, self.value.to_bits());
        abi_put_u32(&mut out, RULE_COMPARE_VALUE2_OFFSET, self.value2.to_bits());
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_COMPARE_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(bytes, RULE_COMPARE_RESERVED_OFFSET, 3)?;
        Ok(Self {
            op: bytes[RULE_COMPARE_OP_OFFSET],
            value: f32::from_bits(abi_get_u32(bytes, RULE_COMPARE_VALUE_OFFSET)),
            value2: f32::from_bits(abi_get_u32(bytes, RULE_COMPARE_VALUE2_OFFSET)),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleCondition {
    pub reference: RuleRef,
    pub compare: RuleCompare,
}

impl RuleCondition {
    const EMPTY: Self = Self {
        reference: RuleRef::EMPTY,
        compare: RuleCompare::EMPTY,
    };

    pub fn encode(self) -> [u8; RULE_CONDITION_LEN] {
        let mut out = [0u8; RULE_CONDITION_LEN];
        out[RULE_CONDITION_REF_OFFSET..RULE_CONDITION_REF_OFFSET + RULE_REF_LEN]
            .copy_from_slice(&self.reference.encode());
        out[RULE_CONDITION_COMPARE_OFFSET..RULE_CONDITION_COMPARE_OFFSET + RULE_COMPARE_LEN]
            .copy_from_slice(&self.compare.encode());
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_CONDITION_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(
            bytes,
            RULE_CONDITION_RESERVED_OFFSET,
            RULE_CONDITION_LEN - RULE_CONDITION_RESERVED_OFFSET,
        )?;
        Ok(Self {
            reference: RuleRef::decode(
                &bytes[RULE_CONDITION_REF_OFFSET..RULE_CONDITION_REF_OFFSET + RULE_REF_LEN],
            )?,
            compare: RuleCompare::decode(
                &bytes[RULE_CONDITION_COMPARE_OFFSET
                    ..RULE_CONDITION_COMPARE_OFFSET + RULE_COMPARE_LEN],
            )?,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleTrigger {
    pub entity: EntityId,
    pub event_id: EventId,
    pub property: PropertyId,
    pub value: f32,
    pub value2: f32,
    pub minutes_of_day: u16,
    pub kind: u8,
    pub op: u8,
    pub edge: u8,
    pub weekday_mask: u8,
}

impl RuleTrigger {
    const EMPTY: Self = Self {
        entity: 0,
        event_id: 0,
        property: 0,
        value: 0.0,
        value2: 0.0,
        minutes_of_day: 0,
        kind: 0,
        op: 0,
        edge: 0,
        weekday_mask: 0,
    };

    pub fn encode(self) -> [u8; RULE_TRIGGER_LEN] {
        let mut out = [0u8; RULE_TRIGGER_LEN];
        abi_put_u64(&mut out, RULE_TRIGGER_ENTITY_OFFSET, self.entity);
        abi_put_u32(&mut out, RULE_TRIGGER_EVENT_ID_OFFSET, self.event_id);
        abi_put_u32(&mut out, RULE_TRIGGER_PROPERTY_OFFSET, self.property);
        abi_put_u32(&mut out, RULE_TRIGGER_VALUE_OFFSET, self.value.to_bits());
        abi_put_u32(&mut out, RULE_TRIGGER_VALUE2_OFFSET, self.value2.to_bits());
        abi_put_u16(&mut out, RULE_TRIGGER_MINUTES_OFFSET, self.minutes_of_day);
        out[RULE_TRIGGER_KIND_OFFSET] = self.kind;
        out[RULE_TRIGGER_OP_OFFSET] = self.op;
        out[RULE_TRIGGER_EDGE_OFFSET] = self.edge;
        out[RULE_TRIGGER_WEEKDAY_OFFSET] = self.weekday_mask;
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_TRIGGER_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(
            bytes,
            RULE_TRIGGER_RESERVED_OFFSET,
            RULE_TRIGGER_LEN - RULE_TRIGGER_RESERVED_OFFSET,
        )?;
        Ok(Self {
            entity: abi_get_u64(bytes, RULE_TRIGGER_ENTITY_OFFSET),
            event_id: abi_get_u32(bytes, RULE_TRIGGER_EVENT_ID_OFFSET),
            property: abi_get_u32(bytes, RULE_TRIGGER_PROPERTY_OFFSET),
            value: f32::from_bits(abi_get_u32(bytes, RULE_TRIGGER_VALUE_OFFSET)),
            value2: f32::from_bits(abi_get_u32(bytes, RULE_TRIGGER_VALUE2_OFFSET)),
            minutes_of_day: abi_get_u16(bytes, RULE_TRIGGER_MINUTES_OFFSET),
            kind: bytes[RULE_TRIGGER_KIND_OFFSET],
            op: bytes[RULE_TRIGGER_OP_OFFSET],
            edge: bytes[RULE_TRIGGER_EDGE_OFFSET],
            weekday_mask: bytes[RULE_TRIGGER_WEEKDAY_OFFSET],
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum RuleCommandValue {
    None,
    Scalar(Value),
    Xy { x: f32, y: f32 },
}

impl RuleCommandValue {
    pub fn encode(self) -> AbiResult<[u8; RULE_COMMAND_VALUE_LEN]> {
        let mut out = [0u8; RULE_COMMAND_VALUE_LEN];
        match self {
            Self::None => {}
            Self::Scalar(value) => {
                if value.kind == ValueType::None {
                    return Err(AbiError::InvalidKind);
                }
                abi_put_u32(&mut out, RULE_COMMAND_KIND_OFFSET, 1);
                abi_put_u32(&mut out, RULE_COMMAND_SCALAR_KIND_OFFSET, value.kind as u32);
                let bytes = value.to_bytes();
                out[RULE_COMMAND_SCALAR_VALUE_OFFSET..].copy_from_slice(&bytes[4..8]);
            }
            Self::Xy { x, y } => {
                abi_put_u32(&mut out, RULE_COMMAND_KIND_OFFSET, 2);
                abi_put_u32(&mut out, RULE_COMMAND_XY_X_OFFSET, x.to_bits());
                abi_put_u32(&mut out, RULE_COMMAND_XY_Y_OFFSET, y.to_bits());
            }
        }
        Ok(out)
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_COMMAND_VALUE_LEN {
            return Err(AbiError::InvalidSize);
        }
        match abi_get_u32(bytes, RULE_COMMAND_KIND_OFFSET) {
            0 => {
                abi_require_zero(
                    bytes,
                    RULE_COMMAND_UNION_OFFSET,
                    RULE_COMMAND_VALUE_LEN - RULE_COMMAND_UNION_OFFSET,
                )
                .map_err(|_| AbiError::NonZeroUnused)?;
                Ok(Self::None)
            }
            1 => {
                let scalar_kind = abi_get_u32(bytes, RULE_COMMAND_SCALAR_KIND_OFFSET);
                if !(1..=5).contains(&scalar_kind) {
                    return Err(AbiError::InvalidKind);
                }
                let bits = abi_get_u32(bytes, RULE_COMMAND_SCALAR_VALUE_OFFSET);
                let mut value = [0u8; RULE_VALUE_LEN];
                value[0] = scalar_kind as u8;
                value[4..8].copy_from_slice(&bits.to_le_bytes());
                Ok(Self::Scalar(
                    Value::from_bytes(&value).ok_or(AbiError::InvalidKind)?,
                ))
            }
            2 => Ok(Self::Xy {
                x: f32::from_bits(abi_get_u32(bytes, RULE_COMMAND_XY_X_OFFSET)),
                y: f32::from_bits(abi_get_u32(bytes, RULE_COMMAND_XY_Y_OFFSET)),
            }),
            _ => Err(AbiError::InvalidKind),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleAction {
    pub reference: RuleRef,
    pub value: RuleCommandValue,
    pub action: u8,
}

impl RuleAction {
    const EMPTY: Self = Self {
        reference: RuleRef::EMPTY,
        value: RuleCommandValue::None,
        action: 0,
    };

    pub fn encode(self) -> AbiResult<[u8; RULE_ACTION_LEN]> {
        let mut out = [0u8; RULE_ACTION_LEN];
        out[RULE_ACTION_REF_OFFSET..RULE_ACTION_REF_OFFSET + RULE_REF_LEN]
            .copy_from_slice(&self.reference.encode());
        out[RULE_ACTION_VALUE_OFFSET..RULE_ACTION_VALUE_OFFSET + RULE_COMMAND_VALUE_LEN]
            .copy_from_slice(&self.value.encode()?);
        out[RULE_ACTION_ACTION_OFFSET] = self.action;
        Ok(out)
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_ACTION_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(
            bytes,
            RULE_ACTION_RESERVED_OFFSET,
            RULE_ACTION_LEN - RULE_ACTION_RESERVED_OFFSET,
        )?;
        Ok(Self {
            reference: RuleRef::decode(
                &bytes[RULE_ACTION_REF_OFFSET..RULE_ACTION_REF_OFFSET + RULE_REF_LEN],
            )?,
            value: RuleCommandValue::decode(
                &bytes[RULE_ACTION_VALUE_OFFSET..RULE_ACTION_VALUE_OFFSET + RULE_COMMAND_VALUE_LEN],
            )?,
            action: bytes[RULE_ACTION_ACTION_OFFSET],
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleRecord {
    pub triggers: [RuleTrigger; RULE_TRIGGERS_MAX],
    pub actions: [RuleAction; RULE_ACTIONS_MAX],
    pub conditions: [RuleCondition; RULE_CONDITIONS_MAX],
    pub enabled: u8,
    pub trigger_count: u8,
    pub action_count: u8,
    pub conditions_count: u8,
    pub name: [u8; RULE_NAME_MAX],
}

impl RuleRecord {
    pub fn encode(self) -> AbiResult<[u8; RULE_RECORD_LEN]> {
        abi_require_count(self.trigger_count, RULE_TRIGGERS_MAX)?;
        abi_require_count(self.action_count, RULE_ACTIONS_MAX)?;
        abi_require_count(self.conditions_count, RULE_CONDITIONS_MAX)?;

        let mut out = [0u8; RULE_RECORD_LEN];
        for (index, trigger) in self.triggers.iter().enumerate() {
            let start = RULE_TRIGGERS_OFFSET + index * RULE_TRIGGER_LEN;
            out[start..start + RULE_TRIGGER_LEN].copy_from_slice(&trigger.encode());
        }
        for (index, action) in self.actions.iter().enumerate() {
            let start = RULE_ACTIONS_OFFSET + index * RULE_ACTION_LEN;
            out[start..start + RULE_ACTION_LEN].copy_from_slice(&action.encode()?);
        }
        for (index, condition) in self.conditions.iter().enumerate() {
            let start = RULE_CONDITIONS_OFFSET + index * RULE_CONDITION_LEN;
            out[start..start + RULE_CONDITION_LEN].copy_from_slice(&condition.encode());
        }
        out[RULE_ENABLED_OFFSET] = self.enabled;
        out[RULE_TRIGGER_COUNT_OFFSET] = self.trigger_count;
        out[RULE_ACTION_COUNT_OFFSET] = self.action_count;
        out[RULE_CONDITIONS_COUNT_OFFSET] = self.conditions_count;
        out[RULE_NAME_OFFSET..].copy_from_slice(&self.name);
        Ok(out)
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_RECORD_LEN {
            return Err(AbiError::InvalidSize);
        }
        let trigger_count = bytes[RULE_TRIGGER_COUNT_OFFSET];
        let action_count = bytes[RULE_ACTION_COUNT_OFFSET];
        let conditions_count = bytes[RULE_CONDITIONS_COUNT_OFFSET];
        abi_require_count(trigger_count, RULE_TRIGGERS_MAX)?;
        abi_require_count(action_count, RULE_ACTIONS_MAX)?;
        abi_require_count(conditions_count, RULE_CONDITIONS_MAX)?;
        abi_require_zero(bytes, RULE_RESERVED_OFFSET, 4)?;

        let mut triggers = [RuleTrigger::EMPTY; RULE_TRIGGERS_MAX];
        for (index, slot) in triggers.iter_mut().enumerate() {
            let start = RULE_TRIGGERS_OFFSET + index * RULE_TRIGGER_LEN;
            *slot = RuleTrigger::decode(&bytes[start..start + RULE_TRIGGER_LEN])?;
        }
        let mut actions = [RuleAction::EMPTY; RULE_ACTIONS_MAX];
        for (index, slot) in actions.iter_mut().enumerate() {
            let start = RULE_ACTIONS_OFFSET + index * RULE_ACTION_LEN;
            *slot = RuleAction::decode(&bytes[start..start + RULE_ACTION_LEN])?;
        }
        let mut conditions = [RuleCondition::EMPTY; RULE_CONDITIONS_MAX];
        for (index, slot) in conditions.iter_mut().enumerate() {
            let start = RULE_CONDITIONS_OFFSET + index * RULE_CONDITION_LEN;
            *slot = RuleCondition::decode(&bytes[start..start + RULE_CONDITION_LEN])?;
        }
        let mut name = [0u8; RULE_NAME_MAX];
        name.copy_from_slice(&bytes[RULE_NAME_OFFSET..]);

        Ok(Self {
            triggers,
            actions,
            conditions,
            enabled: bytes[RULE_ENABLED_OFFSET],
            trigger_count,
            action_count,
            conditions_count,
            name,
        })
    }
}

pub fn set_rule_enabled(record: &[u8], enabled: bool) -> AbiResult<[u8; RULE_RECORD_LEN]> {
    let mut rule = RuleRecord::decode(record)?;
    rule.enabled = u8::from(enabled);
    rule.encode()
}

