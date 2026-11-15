use super::*;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AutomationKey {
    pub id: u64,
}

impl AutomationKey {
    pub fn to_bytes(self) -> [u8; AUTOMATION_KEY_LEN] {
        self.id.to_le_bytes()
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != AUTOMATION_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            id: abi_get_u64(bytes, AUTOMATION_KEY_ID_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupKey {
    pub id: u64,
}

impl GroupKey {
    pub fn to_bytes(self) -> [u8; GROUP_KEY_LEN] {
        self.id.to_le_bytes()
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            id: abi_get_u64(bytes, GROUP_KEY_ID_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupRecord {
    pub title: [u8; GROUP_TITLE_MAX],
}

impl GroupRecord {
    pub fn encode(self) -> [u8; GROUP_RECORD_LEN] {
        self.title
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_RECORD_LEN {
            return Err(AbiError::InvalidSize);
        }
        let mut title = [0u8; GROUP_TITLE_MAX];
        title.copy_from_slice(&bytes[..GROUP_TITLE_MAX]);
        Ok(Self { title })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupItemKey {
    pub group_id: u64,
    pub entity: u64,
}

impl GroupItemKey {
    pub fn to_bytes(self) -> [u8; GROUP_ITEM_KEY_LEN] {
        let mut out = [0u8; GROUP_ITEM_KEY_LEN];
        abi_put_u64(&mut out, GROUP_ITEM_GROUP_OFFSET, self.group_id);
        abi_put_u64(&mut out, GROUP_ITEM_ENTITY_OFFSET, self.entity);
        out
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_ITEM_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            group_id: abi_get_u64(bytes, GROUP_ITEM_GROUP_OFFSET),
            entity: abi_get_u64(bytes, GROUP_ITEM_ENTITY_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupItemRecord {
    pub order: u16,
    pub title: [u8; GROUP_ITEM_TITLE_MAX],
}

impl GroupItemRecord {
    pub fn encode(self) -> [u8; GROUP_ITEM_RECORD_LEN] {
        let mut out = [0u8; GROUP_ITEM_RECORD_LEN];
        abi_put_u16(&mut out, GROUP_ITEM_ORDER_OFFSET, self.order);
        out[GROUP_ITEM_TITLE_OFFSET..].copy_from_slice(&self.title);
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_ITEM_RECORD_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(bytes, GROUP_ITEM_RESERVED_OFFSET, 2)?;
        let mut title = [0u8; GROUP_ITEM_TITLE_MAX];
        title.copy_from_slice(&bytes[GROUP_ITEM_TITLE_OFFSET..]);
        Ok(Self {
            order: abi_get_u16(bytes, GROUP_ITEM_ORDER_OFFSET),
            title,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupAutomationKey {
    pub group_id: u64,
    pub automation_id: u64,
}

impl GroupAutomationKey {
    pub fn to_bytes(self) -> [u8; GROUP_AUTOMATION_KEY_LEN] {
        let mut out = [0u8; GROUP_AUTOMATION_KEY_LEN];
        abi_put_u64(&mut out, GROUP_AUTOMATION_GROUP_OFFSET, self.group_id);
        abi_put_u64(
            &mut out,
            GROUP_AUTOMATION_AUTOMATION_OFFSET,
            self.automation_id,
        );
        out
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_AUTOMATION_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            group_id: abi_get_u64(bytes, GROUP_AUTOMATION_GROUP_OFFSET),
            automation_id: abi_get_u64(bytes, GROUP_AUTOMATION_AUTOMATION_OFFSET),
        })
    }
}
