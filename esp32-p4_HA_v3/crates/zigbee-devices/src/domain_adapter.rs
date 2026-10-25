//! Canonical HA registration and property state over `ha-domain`.
//! Domain is the only reported-state store; this adapter adds no cache.

use ha_domain::canonical::{
    device_id_derive, zigbee_entity_id, DeviceId, EntityId, EntityRecord, LogicalDeviceRecord,
    PropertyId, StateKey, StateRecord, ENTITY_CAPABILITIES, ENTITY_ENTITY,
    ENTITY_LOGICAL_DEVICE, ENTITY_PROPERTY_STATE, SOURCE_ZIGBEE,
};
use ha_domain::{Backing, Domain, EntityDesc, FactMeta, Value as DomainValue};
use z2m_rs::Profile;

use crate::canonical::profile_capabilities;

pub const STATE_CAPACITY: usize = 64;
pub const ENTITY_CAPACITY: usize = 16;
pub const DEVICE_CAPACITY: usize = 16;

pub struct DomainAdapter<'a> {
    domain: &'a mut Domain,
}

impl<'a> DomainAdapter<'a> {
    pub fn new(domain: &'a mut Domain) -> Result<Self, ha_domain::Error> {
        let mut adapter = Self { domain };
        adapter.register_tables()?;
        Ok(adapter)
    }

    fn register_tables(&mut self) -> Result<(), ha_domain::Error> {
        let tables = [
            EntityDesc {
                entity: ENTITY_LOGICAL_DEVICE,
                key_size: 8,
                record_size: ha_domain::canonical::DEVICE_RECORD_LEN,
                capacity: DEVICE_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_ENTITY,
                key_size: 8,
                record_size: ha_domain::canonical::ENTITY_RECORD_LEN,
                capacity: ENTITY_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_CAPABILITIES,
                key_size: 8,
                record_size: ha_domain::canonical::CAPABILITIES_LEN,
                capacity: ENTITY_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_PROPERTY_STATE,
                key_size: ha_domain::canonical::STATE_KEY_LEN,
                record_size: ha_domain::canonical::STATE_RECORD_LEN,
                capacity: STATE_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
        ];
        for desc in tables {
            match self.domain.register_entity(desc) {
                Ok(()) | Err(ha_domain::Error::AlreadyExists) => {}
                Err(error) => return Err(error),
            }
        }
        Ok(())
    }

    pub fn ensure_device(
        &mut self,
        uid: u64,
        name: &str,
    ) -> Result<(DeviceId, bool), ha_domain::Error> {
        let native = format!("{uid:016x}");
        let id = device_id_derive(SOURCE_ZIGBEE, native.as_bytes());
        let record = LogicalDeviceRecord::new(SOURCE_ZIGBEE, 0, &native, name);
        let changed = self.domain.entity_put(
            ENTITY_LOGICAL_DEVICE,
            &id.to_le_bytes(),
            &record.encode(),
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value: DomainValue::NONE,
            },
        )?;
        Ok((id, changed))
    }

    pub fn ensure_entity(
        &mut self,
        uid: u64,
        endpoint: u8,
        name: &str,
        profile: &Profile,
    ) -> Result<(EntityId, bool), ha_domain::Error> {
        let device = device_id_derive(
            SOURCE_ZIGBEE,
            format!("{uid:016x}").as_bytes(),
        );
        let entity = zigbee_entity_id(uid, endpoint);
        let record = EntityRecord::new(device, name);
        let mut changed = self.domain.entity_put(
            ENTITY_ENTITY,
            &entity.to_le_bytes(),
            &record.encode(),
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value: DomainValue::NONE,
            },
        )?;
        let caps = profile_capabilities(profile).encode();
        changed |= self.domain.entity_put(
            ENTITY_CAPABILITIES,
            &entity.to_le_bytes(),
            &caps,
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value: DomainValue::NONE,
            },
        )?;
        Ok((entity, changed))
    }

    pub fn write_state(
        &mut self,
        entity: EntityId,
        property: PropertyId,
        value: DomainValue,
    ) -> Result<bool, ha_domain::Error> {
        let key = StateKey { entity, property }.to_bytes();
        let record = StateRecord { value }.encode();
        self.domain.entity_put(
            ENTITY_PROPERTY_STATE,
            &key,
            &record,
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value,
            },
        )
    }

    pub fn read_state(
        &self,
        entity: EntityId,
        property: PropertyId,
    ) -> Option<DomainValue> {
        let key = StateKey { entity, property }.to_bytes();
        let mut bytes = [0u8; ha_domain::canonical::STATE_RECORD_LEN];
        self.domain
            .entity_get(ENTITY_PROPERTY_STATE, &key, &mut bytes)
            .ok()?;
        DomainValue::from_bytes(&bytes)
    }

    pub fn read_capabilities(&self, entity: EntityId) -> Option<[u8; ha_domain::canonical::CAPABILITIES_LEN]> {
        let mut bytes = [0u8; ha_domain::canonical::CAPABILITIES_LEN];
        self.domain
            .entity_get(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &mut bytes)
            .ok()?;
        Some(bytes)
    }
}
