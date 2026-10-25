//! Canonical HA registration and property state over `ha-domain`.
//! Domain is the only reported-state store; this adapter adds no cache.

use ha_domain::canonical::{
    device_id_derive, zigbee_entity_id, DeviceId, EntityId, EntityRecord, LogicalDeviceRecord,
    PropertyId, StateKey, StateRecord, ENTITY_CAPABILITIES, ENTITY_ENTITY, ENTITY_LOGICAL_DEVICE,
    ENTITY_PROPERTY_STATE, SOURCE_ZIGBEE,
};
use ha_domain::{Backing, Domain, EntityDesc, FactMeta, Value as DomainValue};
use z2m_rs::Profile;

use crate::projection::profile_capabilities;
use crate::standard;

pub const STATE_CAPACITY: usize = 64;
pub const ENTITY_CAPACITY: usize = 16;
pub const DEVICE_CAPACITY: usize = 16;

/// Write-through port for canonical records. The firmware implements it over
/// MicroDB; host tests can use an in-memory sink.
pub trait RecordSink {
    fn put(&mut self, entity: u32, key: &[u8], record: &[u8]);
    fn remove(&mut self, entity: u32, key: &[u8]);
}

pub struct DomainAdapter<'a> {
    domain: &'a mut Domain,
    sink: Option<&'a mut dyn RecordSink>,
}

impl<'a> DomainAdapter<'a> {
    pub fn new(domain: &'a mut Domain) -> Result<Self, ha_domain::Error> {
        let mut adapter = Self { domain, sink: None };
        adapter.register_tables()?;
        Ok(adapter)
    }

    pub fn with_sink(
        domain: &'a mut Domain,
        sink: &'a mut dyn RecordSink,
    ) -> Result<Self, ha_domain::Error> {
        let mut adapter = Self {
            domain,
            sink: Some(sink),
        };
        adapter.register_tables()?;
        Ok(adapter)
    }

    fn persist(&mut self, entity: u32, key: &[u8], record: &[u8]) {
        if let Some(sink) = self.sink.as_mut() {
            sink.put(entity, key, record);
        }
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
        if self.read_device(id).is_some() {
            return Ok((id, false));
        }
        let record = LogicalDeviceRecord::new(SOURCE_ZIGBEE, 0, &native, name);
        let encoded = record.encode();
        let changed = self.domain.entity_put(
            ENTITY_LOGICAL_DEVICE,
            &id.to_le_bytes(),
            &encoded,
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value: DomainValue::NONE,
            },
        )?;
        if changed {
            self.persist(ENTITY_LOGICAL_DEVICE, &id.to_le_bytes(), &encoded);
        }
        Ok((id, changed))
    }

    pub fn ensure_entity(
        &mut self,
        uid: u64,
        endpoint: u8,
        name: &str,
        profile: &Profile,
    ) -> Result<(EntityId, bool), ha_domain::Error> {
        let device = device_id_derive(SOURCE_ZIGBEE, format!("{uid:016x}").as_bytes());
        let entity = zigbee_entity_id(uid, endpoint);
        let mut changed = false;
        if self.read_entity(entity).is_none() {
            let record = EntityRecord::new(device, name);
            let entity_bytes = record.encode();
            changed = self.domain.entity_put(
                ENTITY_ENTITY,
                &entity.to_le_bytes(),
                &entity_bytes,
                FactMeta {
                    source: SOURCE_ZIGBEE as u8,
                    value: DomainValue::NONE,
                },
            )?;
            if changed {
                self.persist(ENTITY_ENTITY, &entity.to_le_bytes(), &entity_bytes);
            }
        }
        let caps = profile_capabilities(profile).encode();
        let caps_changed = self.domain.entity_put(
            ENTITY_CAPABILITIES,
            &entity.to_le_bytes(),
            &caps,
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value: DomainValue::NONE,
            },
        )?;
        if caps_changed {
            self.persist(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &caps);
        }
        Ok((entity, changed || caps_changed))
    }

    pub fn ensure_standard_entity(
        &mut self,
        uid: u64,
        endpoint: u8,
        name: &str,
        descriptor: &crate::EndpointDescriptor,
    ) -> Result<(EntityId, bool), ha_domain::Error> {
        let device = device_id_derive(SOURCE_ZIGBEE, format!("{uid:016x}").as_bytes());
        let entity = zigbee_entity_id(uid, endpoint);
        let mut changed = false;
        if self.read_entity(entity).is_none() {
            let record = EntityRecord::new(device, name);
            let entity_bytes = record.encode();
            changed = self.domain.entity_put(
                ENTITY_ENTITY,
                &entity.to_le_bytes(),
                &entity_bytes,
                FactMeta {
                    source: SOURCE_ZIGBEE as u8,
                    value: DomainValue::NONE,
                },
            )?;
            if changed {
                self.persist(ENTITY_ENTITY, &entity.to_le_bytes(), &entity_bytes);
            }
        }
        // Keep capabilities restored from MicroDB: re-registration must not
        // replace confirmed data with the empty bootstrap set.
        let caps_changed = if self.read_capabilities(entity).is_some() {
            false
        } else {
            let caps =
                standard::capabilities_confirmed(descriptor, &crate::standard::Confirmed::new())
                    .encode();
            let changed = self.domain.entity_put(
                ENTITY_CAPABILITIES,
                &entity.to_le_bytes(),
                &caps,
                FactMeta {
                    source: SOURCE_ZIGBEE as u8,
                    value: DomainValue::NONE,
                },
            )?;
            if changed {
                self.persist(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &caps);
            }
            changed
        };
        Ok((entity, changed || caps_changed))
    }

    pub fn write_capabilities(
        &mut self,
        entity: EntityId,
        capabilities: &ha_domain::canonical::CapabilitiesRecord,
    ) -> Result<bool, ha_domain::Error> {
        let encoded = capabilities.encode();
        let changed = self.domain.entity_put(
            ENTITY_CAPABILITIES,
            &entity.to_le_bytes(),
            &encoded,
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value: DomainValue::NONE,
            },
        )?;
        if changed {
            self.persist(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &encoded);
        }
        Ok(changed)
    }

    pub fn write_state(
        &mut self,
        entity: EntityId,
        property: PropertyId,
        value: DomainValue,
    ) -> Result<bool, ha_domain::Error> {
        let key = StateKey { entity, property }.to_bytes();
        let record = StateRecord { value }.encode();
        let changed = self.domain.entity_put(
            ENTITY_PROPERTY_STATE,
            &key,
            &record,
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value,
            },
        )?;
        if changed {
            self.persist(ENTITY_PROPERTY_STATE, &key, &record);
        }
        Ok(changed)
    }

    pub fn read_state(&self, entity: EntityId, property: PropertyId) -> Option<DomainValue> {
        let key = StateKey { entity, property }.to_bytes();
        let mut bytes = [0u8; ha_domain::canonical::STATE_RECORD_LEN];
        self.domain
            .entity_get(ENTITY_PROPERTY_STATE, &key, &mut bytes)
            .ok()?;
        DomainValue::from_bytes(&bytes)
    }

    pub fn read_capabilities(
        &self,
        entity: EntityId,
    ) -> Option<[u8; ha_domain::canonical::CAPABILITIES_LEN]> {
        let mut bytes = [0u8; ha_domain::canonical::CAPABILITIES_LEN];
        self.domain
            .entity_get(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &mut bytes)
            .ok()?;
        Some(bytes)
    }

    pub fn read_entity(
        &self,
        entity: EntityId,
    ) -> Option<[u8; ha_domain::canonical::ENTITY_RECORD_LEN]> {
        let mut bytes = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
        self.domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut bytes)
            .ok()?;
        Some(bytes)
    }

    pub fn read_device(
        &self,
        device: DeviceId,
    ) -> Option<[u8; ha_domain::canonical::DEVICE_RECORD_LEN]> {
        let mut bytes = [0u8; ha_domain::canonical::DEVICE_RECORD_LEN];
        self.domain
            .entity_get(ENTITY_LOGICAL_DEVICE, &device.to_le_bytes(), &mut bytes)
            .ok()?;
        Some(bytes)
    }

    pub fn rename_entity(
        &mut self,
        entity: EntityId,
        name: &str,
    ) -> Result<bool, ha_domain::Error> {
        let bytes = self.read_entity(entity).ok_or(ha_domain::Error::NotFound)?;
        let device = u64::from_le_bytes(bytes[..8].try_into().expect("entity device"));
        let record = EntityRecord::new(device, name);
        let encoded = record.encode();
        let changed =
            self.domain
                .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &encoded, fact_meta())?;
        if changed {
            self.persist(ENTITY_ENTITY, &entity.to_le_bytes(), &encoded);
        }
        Ok(changed)
    }

    pub fn rename_device(
        &mut self,
        device: DeviceId,
        name: &str,
    ) -> Result<bool, ha_domain::Error> {
        let bytes = self.read_device(device).ok_or(ha_domain::Error::NotFound)?;
        let parent = u64::from_le_bytes(bytes[..8].try_into().expect("device parent"));
        let source = u32::from_le_bytes(bytes[8..12].try_into().expect("device source"));
        let native = std::str::from_utf8(&bytes[12..44])
            .unwrap_or("")
            .trim_end_matches('\0');
        let record = LogicalDeviceRecord::new(source, parent, native, name);
        let encoded = record.encode();
        let changed = self.domain.entity_put(
            ENTITY_LOGICAL_DEVICE,
            &device.to_le_bytes(),
            &encoded,
            fact_meta(),
        )?;
        if changed {
            self.persist(ENTITY_LOGICAL_DEVICE, &device.to_le_bytes(), &encoded);
        }
        Ok(changed)
    }

    pub fn remove_entity(&mut self, entity: EntityId) -> Result<(), ha_domain::Error> {
        let key = entity.to_le_bytes();
        self.domain
            .entity_remove(ENTITY_ENTITY, &key, fact_meta())?;
        self.remove_persist(ENTITY_ENTITY, &key);
        Ok(())
    }

    pub fn remove_capabilities(&mut self, entity: EntityId) -> Result<(), ha_domain::Error> {
        let key = entity.to_le_bytes();
        self.domain
            .entity_remove(ENTITY_CAPABILITIES, &key, fact_meta())?;
        self.remove_persist(ENTITY_CAPABILITIES, &key);
        Ok(())
    }

    fn remove_states(&mut self, entity: EntityId) -> Result<(), ha_domain::Error> {
        let mut keys: Vec<Vec<u8>> = Vec::new();
        self.domain.entity_iter(ENTITY_PROPERTY_STATE, |key, _| {
            if key.len() >= 8
                && u64::from_le_bytes(key[..8].try_into().expect("state entity")) == entity
            {
                keys.push(key.to_vec());
            }
        })?;
        for key in keys {
            if self
                .domain
                .entity_remove(ENTITY_PROPERTY_STATE, &key, fact_meta())
                .is_ok()
            {
                self.remove_persist(ENTITY_PROPERTY_STATE, &key);
            }
        }
        Ok(())
    }

    /// Removes every record of one logical device and returns how many rows
    /// were removed. The caller mirrors the same removals to the web model.
    pub fn remove_device_records(&mut self, device: DeviceId) -> Result<usize, ha_domain::Error> {
        let mut entities: Vec<u64> = Vec::new();
        self.domain.entity_iter(ENTITY_ENTITY, |key, record| {
            if key.len() == 8
                && record.len() >= 8
                && u64::from_le_bytes(record[..8].try_into().expect("entity device")) == device
            {
                entities.push(u64::from_le_bytes(key.try_into().expect("entity key")));
            }
        })?;
        let mut removed = 0;
        for entity in entities {
            if self.remove_entity(entity).is_ok() {
                removed += 1;
            }
            let _ = self.remove_capabilities(entity);
            let _ = self.remove_states(entity);
        }
        let key = device.to_le_bytes();
        if self
            .domain
            .entity_remove(ENTITY_LOGICAL_DEVICE, &key, fact_meta())
            .is_ok()
        {
            self.remove_persist(ENTITY_LOGICAL_DEVICE, &key);
            removed += 1;
        }
        Ok(removed)
    }

    fn remove_persist(&mut self, entity: u32, key: &[u8]) {
        if let Some(sink) = self.sink.as_mut() {
            sink.remove(entity, key);
        }
    }
}

fn fact_meta() -> FactMeta {
    FactMeta {
        source: SOURCE_ZIGBEE as u8,
        value: DomainValue::NONE,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::canonical::PROPERTY_POWER;
    use std::cell::RefCell;
    use std::rc::Rc;

    struct Sink {
        puts: Rc<RefCell<Vec<(u32, Vec<u8>, Vec<u8>)>>>,
        removes: Rc<RefCell<Vec<(u32, Vec<u8>)>>>,
    }

    impl RecordSink for Sink {
        fn put(&mut self, entity: u32, key: &[u8], record: &[u8]) {
            self.puts
                .borrow_mut()
                .push((entity, key.to_vec(), record.to_vec()));
        }

        fn remove(&mut self, entity: u32, key: &[u8]) {
            self.removes.borrow_mut().push((entity, key.to_vec()));
        }
    }

    #[test]
    fn changed_state_is_written_through_and_unchanged_is_not() {
        let mut domain = Domain::new(8, 2, 8).unwrap();
        let puts = Rc::new(RefCell::new(Vec::new()));
        let removes = Rc::new(RefCell::new(Vec::new()));
        let mut sink = Sink {
            puts: Rc::clone(&puts),
            removes,
        };
        let mut adapter = DomainAdapter::with_sink(&mut domain, &mut sink).unwrap();
        assert!(adapter
            .write_state(7, PROPERTY_POWER, DomainValue::bool(true))
            .unwrap());
        assert_eq!(puts.borrow().len(), 1);
        assert_eq!(puts.borrow()[0].0, ENTITY_PROPERTY_STATE);
        assert_eq!(puts.borrow()[0].2[0], 1, "BOOL value kind");

        assert!(!adapter
            .write_state(7, PROPERTY_POWER, DomainValue::bool(true))
            .unwrap());
        assert_eq!(puts.borrow().len(), 1);
    }

    #[test]
    fn rename_and_remove_update_domain_and_the_sink() {
        let uid = 0x1122_3344_5566_7788;
        let mut domain = Domain::new(8, 2, 8).unwrap();
        let puts = Rc::new(RefCell::new(Vec::new()));
        let removes = Rc::new(RefCell::new(Vec::new()));
        let mut sink = Sink {
            puts,
            removes: Rc::clone(&removes),
        };
        let mut adapter = DomainAdapter::with_sink(&mut domain, &mut sink).unwrap();
        let (device, _) = adapter.ensure_device(uid, "old").unwrap();
        let (entity, _) = adapter
            .ensure_entity(uid, 1, "old", &z2m_rs::TUYA_TS0041)
            .unwrap();
        adapter
            .write_state(entity, PROPERTY_POWER, DomainValue::bool(true))
            .unwrap();

        adapter.rename_entity(entity, "new").unwrap();
        let record = adapter.read_entity(entity).unwrap();
        assert_eq!(&record[8..11], b"new");

        adapter.rename_device(device, "lamp").unwrap();
        let record = adapter.read_device(device).unwrap();
        assert_eq!(&record[44..48], b"lamp");

        assert!(adapter.remove_device_records(device).unwrap() >= 1);
        assert!(adapter.read_entity(entity).is_none());
        assert!(adapter.read_device(device).is_none());
        assert!(removes
            .borrow()
            .iter()
            .any(|(entity_type, _)| *entity_type == ENTITY_LOGICAL_DEVICE));
        assert!(removes
            .borrow()
            .iter()
            .any(|(entity_type, _)| *entity_type == ENTITY_PROPERTY_STATE));
    }
}
