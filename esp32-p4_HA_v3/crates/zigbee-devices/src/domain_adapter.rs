//! Canonical HA registration and property state over `ha-domain`.
//! Domain is the only reported-state store; this adapter adds no cache.

use ha_domain::canonical::{
    device_id_derive, zigbee_entity_id, DeviceId, EntityId, EntityRecord, LogicalDeviceRecord,
    PropertyId, StateKey, StateRecord, AUTOMATION_RECORD_LEN, ENTITY_AUTOMATION,
    ENTITY_CAPABILITIES, ENTITY_ENTITY, ENTITY_GROUP, ENTITY_GROUP_AUTOMATION, ENTITY_GROUP_ITEM,
    ENTITY_LOCATION, ENTITY_LOGICAL_DEVICE, ENTITY_PROPERTY_STATE, ENTITY_SETTINGS, ENTITY_WEATHER,
    ENTITY_WIFI_KNOWN, ENTITY_WIFI_SCAN, ENTITY_WIFI_STATUS, GROUP_ITEM_KEY_LEN,
    GROUP_ITEM_RECORD_LEN, GROUP_KEY_LEN, GROUP_RECORD_LEN, LOCATION_KEY_LEN, LOCATION_RECORD_LEN,
    SETTINGS_RECORD_LEN, SOURCE_ZIGBEE, WEATHER_RECORD_LEN, WIFI_KNOWN_MAX,
    WIFI_KNOWN_RECORD_LEN, WIFI_SCAN_RECORD_LEN, WIFI_STATUS_RECORD_LEN,
};
use ha_domain::{Backing, Domain, EntityDesc, FactMeta, Value as DomainValue};
use z2m_rs::Profile;

use crate::projection::profile_capabilities;
use crate::standard;

// V2 capacity budget (esp32-p4_HA_v2/main/app_main.c). Fixed capacity is
// deliberate: predictable RAM and no hidden allocation. Size changes belong
// in the composition root, not in individual services.
pub const STATE_CAPACITY: usize = 256;
pub const ENTITY_CAPACITY: usize = 32;
pub const DEVICE_CAPACITY: usize = 32;
pub const GROUP_CAPACITY: usize = 16;
pub const GROUP_ITEM_CAPACITY: usize = 128;
pub const GROUP_AUTOMATION_CAPACITY: usize = 32;
pub const AUTOMATION_CAPACITY: usize = 32;
pub const LOCATION_CAPACITY: usize = 4;
pub const WEATHER_CAPACITY: usize = 1;
pub const SETTINGS_CAPACITY: usize = 1;

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
        if entity == ENTITY_PROPERTY_STATE {
            return;
        }
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
            EntityDesc {
                entity: ENTITY_GROUP,
                key_size: GROUP_KEY_LEN,
                record_size: GROUP_RECORD_LEN,
                capacity: GROUP_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_GROUP_ITEM,
                key_size: GROUP_ITEM_KEY_LEN,
                record_size: GROUP_ITEM_RECORD_LEN,
                capacity: GROUP_ITEM_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_GROUP_AUTOMATION,
                key_size: GROUP_ITEM_KEY_LEN,
                record_size: GROUP_ITEM_RECORD_LEN,
                capacity: GROUP_AUTOMATION_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_AUTOMATION,
                key_size: GROUP_KEY_LEN,
                record_size: AUTOMATION_RECORD_LEN,
                capacity: AUTOMATION_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_LOCATION,
                key_size: LOCATION_KEY_LEN,
                record_size: LOCATION_RECORD_LEN,
                capacity: LOCATION_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_WEATHER,
                key_size: 8,
                record_size: WEATHER_RECORD_LEN,
                capacity: WEATHER_CAPACITY,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_WIFI_SCAN,
                key_size: 8,
                record_size: WIFI_SCAN_RECORD_LEN,
                capacity: 1,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_WIFI_KNOWN,
                key_size: 4,
                record_size: WIFI_KNOWN_RECORD_LEN,
                capacity: WIFI_KNOWN_MAX,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_WIFI_STATUS,
                key_size: 8,
                record_size: WIFI_STATUS_RECORD_LEN,
                capacity: 1,
                backing: Backing::Ram,
                persist_key: None,
            },
            EntityDesc {
                entity: ENTITY_SETTINGS,
                key_size: 1,
                record_size: SETTINGS_RECORD_LEN,
                capacity: SETTINGS_CAPACITY,
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
        // PROPERTY_STATE is the live truth in RAM. It is restored by device
        // reports after boot, never by flash. Persistent config is written by
        // the other Domain records only.
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
            let _ = self
                .domain
                .entity_remove(ENTITY_PROPERTY_STATE, &key, fact_meta());
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
            let _ = self.remove_group_items(entity);
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

    /// Generic canonical row, used by groups, automation rules and future types.
    pub fn put_record(
        &mut self,
        entity: u32,
        key: &[u8],
        record: &[u8],
    ) -> Result<bool, ha_domain::Error> {
        let changed = self.domain.entity_put(entity, key, record, fact_meta())?;
        if changed {
            self.persist(entity, key, record);
        }
        Ok(changed)
    }

    pub fn remove_record(&mut self, entity: u32, key: &[u8]) -> Result<bool, ha_domain::Error> {
        match self.domain.entity_remove(entity, key, fact_meta()) {
            Ok(()) => {
                self.remove_persist(entity, key);
                Ok(true)
            }
            Err(ha_domain::Error::NotFound) => Ok(false),
            Err(error) => Err(error),
        }
    }

    /// V2 `cascade_entity`: drop group tiles that reference a removed entity.
    fn remove_group_items(&mut self, entity: EntityId) -> Result<(), ha_domain::Error> {
        let entity_bytes = entity.to_le_bytes();
        let mut keys = Vec::new();
        match self.domain.entity_iter(ENTITY_GROUP_ITEM, |key, _| {
            if key.len() == GROUP_ITEM_KEY_LEN && key[8..16] == entity_bytes[..] {
                keys.push(key.to_vec());
            }
        }) {
            Ok(()) | Err(ha_domain::Error::NotFound) => {}
            Err(error) => return Err(error),
        }
        for key in keys {
            let _ = self.remove_record(ENTITY_GROUP_ITEM, &key);
        }
        Ok(())
    }

    pub fn remove_group(&mut self, group: u64) -> Result<usize, ha_domain::Error> {
        let group_bytes = group.to_le_bytes();
        let mut removed = 0;
        for entity in [ENTITY_GROUP_ITEM, ENTITY_GROUP_AUTOMATION] {
            let mut keys = Vec::new();
            self.domain.entity_iter(entity, |key, _| {
                if key.len() == GROUP_ITEM_KEY_LEN && key[..8] == group_bytes[..] {
                    keys.push(key.to_vec());
                }
            })?;
            for key in keys {
                if self.remove_record(entity, &key)? {
                    removed += 1;
                }
            }
        }
        if self.remove_record(ENTITY_GROUP, &group_bytes)? {
            removed += 1;
        }
        Ok(removed)
    }

    pub fn remove_automation(&mut self, automation: u64) -> Result<usize, ha_domain::Error> {
        let id_bytes = automation.to_le_bytes();
        let mut removed = 0;
        let mut keys = Vec::new();
        self.domain.entity_iter(ENTITY_GROUP_AUTOMATION, |key, _| {
            if key.len() == GROUP_ITEM_KEY_LEN && key[8..16] == id_bytes[..] {
                keys.push(key.to_vec());
            }
        })?;
        for key in keys {
            if self.remove_record(ENTITY_GROUP_AUTOMATION, &key)? {
                removed += 1;
            }
        }
        if self.remove_record(ENTITY_AUTOMATION, &id_bytes)? {
            removed += 1;
        }
        Ok(removed)
    }

    fn remove_persist(&mut self, entity: u32, key: &[u8]) {
        if entity == ENTITY_PROPERTY_STATE {
            return;
        }
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
    use ha_domain::canonical::{LocationRecord, PROPERTY_POWER, SYSTEM_DEVICE_UID};
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
    fn changed_state_stays_in_ram_and_unchanged_is_not_reported() {
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
        assert!(
            puts.borrow().is_empty(),
            "PROPERTY_STATE must not be written through to persistent storage"
        );

        assert!(!adapter
            .write_state(7, PROPERTY_POWER, DomainValue::bool(true))
            .unwrap());
        assert!(puts.borrow().is_empty());
    }

    #[test]
    fn generic_record_path_cannot_persist_state() {
        let mut domain = Domain::new(8, 2, 8).unwrap();
        let puts = Rc::new(RefCell::new(Vec::new()));
        let mut sink = Sink {
            puts: Rc::clone(&puts),
            removes: Rc::new(RefCell::new(Vec::new())),
        };
        let mut adapter = DomainAdapter::with_sink(&mut domain, &mut sink).unwrap();
        let key = StateKey {
            entity: 7,
            property: PROPERTY_POWER,
        }
        .to_bytes();
        let record = StateRecord {
            value: DomainValue::bool(true),
        }
        .encode();
        adapter
            .put_record(ENTITY_PROPERTY_STATE, &key, &record)
            .unwrap();
        drop(adapter);
        assert!(puts.borrow().is_empty());
    }

    #[test]
    fn location_is_written_through_and_read_back() {
        let mut domain = Domain::new(8, 2, 8).unwrap();
        let puts = Rc::new(RefCell::new(Vec::new()));
        let mut sink = Sink {
            puts: Rc::clone(&puts),
            removes: Rc::new(RefCell::new(Vec::new())),
        };
        let mut adapter = DomainAdapter::with_sink(&mut domain, &mut sink).unwrap();
        let key = SYSTEM_DEVICE_UID.to_le_bytes();
        let mut name = [0u8; ha_domain::canonical::LOCATION_NAME_MAX];
        name[..6].copy_from_slice(b"Moscow");
        let record = LocationRecord {
            latitude: 55.75,
            longitude: 37.62,
            tz_offset_min: 180,
            tz_auto: false,
            pos_auto: false,
            name,
        }
        .encode();
        assert!(adapter.put_record(ENTITY_LOCATION, &key, &record).unwrap());
        drop(adapter);

        assert!(puts.borrow().iter().any(|(entity, put_key, put)| {
            *entity == ENTITY_LOCATION
                && put_key.as_slice() == key.as_slice()
                && put.as_slice() == record.as_slice()
        }));
        let mut saved = [0u8; LOCATION_RECORD_LEN];
        domain
            .entity_get(ENTITY_LOCATION, &key, &mut saved)
            .unwrap();
        assert_eq!(saved, record);
    }

    #[test]
    fn group_and_automation_removal_cascade_their_links() {
        let mut domain = Domain::new(8, 2, 8).unwrap();
        let mut sink = Sink {
            puts: Rc::new(RefCell::new(Vec::new())),
            removes: Rc::new(RefCell::new(Vec::new())),
        };
        let mut adapter = DomainAdapter::with_sink(&mut domain, &mut sink).unwrap();
        let group = 0x1111u64;
        let entity = 0x2222u64;
        let automation = 0x3333u64;
        adapter
            .put_record(ENTITY_GROUP, &group.to_le_bytes(), &[1u8; GROUP_RECORD_LEN])
            .unwrap();
        let mut item_key = [0u8; GROUP_ITEM_KEY_LEN];
        item_key[..8].copy_from_slice(&group.to_le_bytes());
        item_key[8..].copy_from_slice(&entity.to_le_bytes());
        adapter
            .put_record(ENTITY_GROUP_ITEM, &item_key, &[2u8; GROUP_ITEM_RECORD_LEN])
            .unwrap();
        adapter
            .put_record(
                ENTITY_AUTOMATION,
                &automation.to_le_bytes(),
                &[3u8; AUTOMATION_RECORD_LEN],
            )
            .unwrap();
        let mut link_key = [0u8; GROUP_ITEM_KEY_LEN];
        link_key[..8].copy_from_slice(&group.to_le_bytes());
        link_key[8..].copy_from_slice(&automation.to_le_bytes());
        adapter
            .put_record(
                ENTITY_GROUP_AUTOMATION,
                &link_key,
                &[4u8; GROUP_ITEM_RECORD_LEN],
            )
            .unwrap();

        assert!(adapter.remove_group(group).unwrap() >= 3);
        assert!(adapter.remove_automation(automation).unwrap() >= 1);
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
        assert!(adapter.read_state(entity, PROPERTY_POWER).is_none());
        assert!(removes
            .borrow()
            .iter()
            .any(|(entity_type, _)| *entity_type == ENTITY_LOGICAL_DEVICE));
    }
}
