//! Neutral Device service: canonical Logical Device lifecycle and ownership.
//!
//! It knows only opaque source/native ids and canonical records; the adapter owns
//! the physical representation and registers a remover for its source.

use std::sync::Mutex;

use ha_domain::canonical::{
    device_id_derive, DeviceId, EntityId, LogicalDeviceRecord, SourceId, DEVICE_RECORD_LEN,
    ENTITY_CAPABILITIES, ENTITY_ENTITY, ENTITY_GROUP_ITEM, ENTITY_LOGICAL_DEVICE,
    ENTITY_PROPERTY_STATE, GROUP_ITEM_KEY_LEN, STATE_KEY_LEN,
};
use ha_domain::Error;
use ha_domain::{Domain, FactMeta, Value};

pub type Operation = fn(&[u8]) -> Result<(), Error>;

const REMOVERS_MAX: usize = 4;

struct Remover {
    source: SourceId,
    operation: Operation,
}

static REMOVERS: Mutex<Vec<Remover>> = Mutex::new(Vec::new());

pub trait Sink {
    fn put(&mut self, entity: u32, key: &[u8], record: &[u8]);
    fn remove(&mut self, entity: u32, key: &[u8]);
}

pub struct NoSink;

impl Sink for NoSink {
    fn put(&mut self, _entity: u32, _key: &[u8], _record: &[u8]) {}
    fn remove(&mut self, _entity: u32, _key: &[u8]) {}
}

#[derive(Clone, Copy)]
pub struct DeviceDesc<'a> {
    pub source: SourceId,
    pub native_id: &'a str,
    pub parent_native_id: Option<&'a str>,
    pub name: &'a str,
}

pub fn register_remover(source: SourceId, operation: Operation) -> Result<(), Error> {
    if source == 0 {
        return Err(Error::InvalidArg);
    }
    let mut removers = REMOVERS.lock().unwrap();
    if let Some(existing) = removers.iter_mut().find(|entry| entry.source == source) {
        existing.operation = operation;
        return Ok(());
    }
    if removers.len() == REMOVERS_MAX {
        return Err(Error::NoSpace);
    }
    removers.push(Remover { source, operation });
    Ok(())
}

fn remover(source: SourceId) -> Option<Operation> {
    REMOVERS
        .lock()
        .unwrap()
        .iter()
        .find(|entry| entry.source == source)
        .map(|entry| entry.operation)
}

fn meta(origin: u8) -> FactMeta {
    FactMeta {
        source: origin,
        value: Value::NONE,
    }
}

fn put_name(name: &mut [u8; 32], value: &str) {
    name.fill(0);
    let bytes = value.as_bytes();
    name[..bytes.len().min(32)].copy_from_slice(&bytes[..bytes.len().min(32)]);
}

fn decode_device(bytes: &[u8; DEVICE_RECORD_LEN]) -> LogicalDeviceRecord {
    let mut native_id = [0u8; 32];
    let mut name = [0u8; 32];
    native_id.copy_from_slice(&bytes[12..44]);
    name.copy_from_slice(&bytes[44..76]);
    LogicalDeviceRecord {
        parent: u64::from_le_bytes(bytes[..8].try_into().expect("device parent")),
        source: u32::from_le_bytes(bytes[8..12].try_into().expect("device source")),
        native_id,
        name,
    }
}

pub fn register(
    domain: &mut Domain,
    origin: u8,
    desc: &DeviceDesc<'_>,
    sink: &mut dyn Sink,
) -> Result<DeviceId, Error> {
    if desc.source == 0 || desc.native_id.is_empty() {
        return Err(Error::InvalidArg);
    }
    let id = device_id_derive(desc.source, desc.native_id.as_bytes());
    if id == 0 {
        return Err(Error::InvalidArg);
    }
    let parent = match desc.parent_native_id.filter(|value| !value.is_empty()) {
        Some(native) => device_id_derive(desc.source, native.as_bytes()),
        None => 0,
    };
    if desc.parent_native_id.is_some_and(|value| !value.is_empty()) && parent == 0 {
        return Err(Error::InvalidArg);
    }

    let key = id.to_le_bytes();
    let mut current = [0u8; DEVICE_RECORD_LEN];
    if domain
        .entity_get(ENTITY_LOGICAL_DEVICE, &key, &mut current)
        .is_ok()
    {
        let mut record = decode_device(&current);
        if record.name[0] == 0 && !desc.name.is_empty() {
            put_name(&mut record.name, desc.name);
            let encoded = record.encode();
            if domain.entity_put(ENTITY_LOGICAL_DEVICE, &key, &encoded, meta(origin))? {
                sink.put(ENTITY_LOGICAL_DEVICE, &key, &encoded);
            }
        }
        return Ok(id);
    }

    let record = LogicalDeviceRecord::new(desc.source, parent, desc.native_id, desc.name);
    let encoded = record.encode();
    if domain.entity_put(ENTITY_LOGICAL_DEVICE, &key, &encoded, meta(origin))? {
        sink.put(ENTITY_LOGICAL_DEVICE, &key, &encoded);
    }
    Ok(id)
}

pub fn bind_entity(
    domain: &mut Domain,
    origin: u8,
    device: DeviceId,
    entity: EntityId,
    sink: &mut dyn Sink,
) -> Result<bool, Error> {
    if device == 0 || entity == 0 {
        return Err(Error::InvalidArg);
    }
    let key = entity.to_le_bytes();
    let mut record = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
    domain.entity_get(ENTITY_ENTITY, &key, &mut record)?;
    let owner = u64::from_le_bytes(record[..8].try_into().expect("entity owner"));
    if owner == device {
        return Ok(false);
    }
    if owner != 0 {
        return Err(Error::InvalidState);
    }
    record[..8].copy_from_slice(&device.to_le_bytes());
    let changed = domain.entity_put(ENTITY_ENTITY, &key, &record, meta(origin))?;
    if changed {
        sink.put(ENTITY_ENTITY, &key, &record);
    }
    Ok(changed)
}

pub fn rename(
    domain: &mut Domain,
    origin: u8,
    device: DeviceId,
    name: &str,
    sink: &mut dyn Sink,
) -> Result<bool, Error> {
    if device == 0 {
        return Err(Error::InvalidArg);
    }
    let key = device.to_le_bytes();
    let mut current = [0u8; DEVICE_RECORD_LEN];
    domain.entity_get(ENTITY_LOGICAL_DEVICE, &key, &mut current)?;
    let mut record = decode_device(&current);
    put_name(&mut record.name, name);
    let encoded = record.encode();
    let changed = domain.entity_put(ENTITY_LOGICAL_DEVICE, &key, &encoded, meta(origin))?;
    if changed {
        sink.put(ENTITY_LOGICAL_DEVICE, &key, &encoded);
    }
    Ok(changed)
}

fn cascade_entity(
    domain: &mut Domain,
    origin: u8,
    entity: EntityId,
    sink: &mut dyn Sink,
) -> Result<(), Error> {
    let entity_bytes = entity.to_le_bytes();
    let states = {
        let mut keys = Vec::new();
        domain.entity_iter(ENTITY_PROPERTY_STATE, |key, _| {
            if key.len() == STATE_KEY_LEN && key[..8] == entity_bytes {
                keys.push(key.to_vec());
            }
        })?;
        keys
    };
    for key in states {
        let _ = domain.entity_remove(ENTITY_PROPERTY_STATE, &key, meta(origin));
    }

    let group_items = {
        let mut keys = Vec::new();
        domain.entity_iter(ENTITY_GROUP_ITEM, |key, _| {
            if key.len() == GROUP_ITEM_KEY_LEN && key[8..16] == entity_bytes {
                keys.push(key.to_vec());
            }
        })?;
        keys
    };
    for key in group_items {
        if domain
            .entity_remove(ENTITY_GROUP_ITEM, &key, meta(origin))
            .is_ok()
        {
            sink.remove(ENTITY_GROUP_ITEM, &key);
        }
    }

    if domain
        .entity_remove(ENTITY_CAPABILITIES, &entity_bytes, meta(origin))
        .is_ok()
    {
        sink.remove(ENTITY_CAPABILITIES, &entity_bytes);
    }
    if domain
        .entity_remove(ENTITY_ENTITY, &entity_bytes, meta(origin))
        .is_ok()
    {
        sink.remove(ENTITY_ENTITY, &entity_bytes);
    }
    Ok(())
}

pub fn remove(
    domain: &mut Domain,
    origin: u8,
    device: DeviceId,
    sink: &mut dyn Sink,
) -> Result<usize, Error> {
    if device == 0 {
        return Err(Error::InvalidArg);
    }
    let key = device.to_le_bytes();
    let mut current = [0u8; DEVICE_RECORD_LEN];
    domain.entity_get(ENTITY_LOGICAL_DEVICE, &key, &mut current)?;
    let record = decode_device(&current);
    let remove_native = remover(record.source).ok_or(Error::InvalidState)?;

    let native_len = record
        .native_id
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(record.native_id.len());
    remove_native(&record.native_id[..native_len])?;

    let owned = {
        let mut keys = Vec::new();
        domain.entity_iter(ENTITY_ENTITY, |key, value| {
            if value.len() >= 8
                && u64::from_le_bytes(value[..8].try_into().expect("entity owner")) == device
            {
                keys.push(key.to_vec());
            }
        })?;
        keys
    };
    let mut removed = 0;
    for key in owned {
        let entity = u64::from_le_bytes(key[..8].try_into().expect("entity key"));
        cascade_entity(domain, origin, entity, sink)?;
        removed += 1;
    }

    if domain
        .entity_remove(ENTITY_LOGICAL_DEVICE, &key, meta(origin))
        .is_ok()
    {
        sink.remove(ENTITY_LOGICAL_DEVICE, &key);
        removed += 1;
    }
    Ok(removed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::{Backing, EntityDesc};
    use std::sync::atomic::{AtomicUsize, Ordering};

    static REMOVED: AtomicUsize = AtomicUsize::new(0);

    fn remover(_native: &[u8]) -> Result<(), Error> {
        REMOVED.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn domain() -> Domain {
        let mut domain = Domain::new(8, 4, 32).unwrap();
        for (entity, key, record, capacity) in [
            (ENTITY_LOGICAL_DEVICE, 8, DEVICE_RECORD_LEN, 8),
            (ENTITY_ENTITY, 8, ha_domain::canonical::ENTITY_RECORD_LEN, 8),
            (
                ENTITY_CAPABILITIES,
                8,
                ha_domain::canonical::CAPABILITIES_LEN,
                8,
            ),
            (ENTITY_PROPERTY_STATE, STATE_KEY_LEN, 8, 8),
            (ENTITY_GROUP_ITEM, GROUP_ITEM_KEY_LEN, 36, 8),
        ] {
            domain
                .register_entity(EntityDesc {
                    entity,
                    key_size: key,
                    record_size: record,
                    capacity,
                    backing: Backing::Ram,
                    persist_key: None,
                })
                .unwrap();
        }
        domain
    }

    #[test]
    fn register_is_idempotent_and_keeps_the_initial_name() {
        let mut domain = domain();
        let mut sink = NoSink;
        let desc = DeviceDesc {
            source: 1,
            native_id: "abc",
            parent_native_id: None,
            name: "lamp",
        };
        let id = register(&mut domain, 2, &desc, &mut sink).unwrap();
        let renamed = DeviceDesc {
            name: "other",
            ..desc
        };
        assert_eq!(register(&mut domain, 2, &renamed, &mut sink), Ok(id));
        let key = id.to_le_bytes();
        let mut bytes = [0u8; DEVICE_RECORD_LEN];
        domain
            .entity_get(ENTITY_LOGICAL_DEVICE, &key, &mut bytes)
            .unwrap();
        assert_eq!(&bytes[44..48], b"lamp");
    }

    #[test]
    fn bind_rejects_reassignment_and_missing_entities() {
        let mut domain = domain();
        let mut sink = NoSink;
        let a = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "a",
                parent_native_id: None,
                name: "",
            },
            &mut sink,
        )
        .unwrap();
        let b = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "b",
                parent_native_id: None,
                name: "",
            },
            &mut sink,
        )
        .unwrap();
        let entity: u64 = 7;
        let mut record = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
        domain
            .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &record, meta(2))
            .unwrap();
        assert_eq!(bind_entity(&mut domain, 2, a, entity, &mut sink), Ok(true));
        assert_eq!(bind_entity(&mut domain, 2, a, entity, &mut sink), Ok(false));
        assert_eq!(
            bind_entity(&mut domain, 2, b, entity, &mut sink),
            Err(Error::InvalidState)
        );
        record[..8].copy_from_slice(&a.to_le_bytes());
        assert_eq!(
            bind_entity(&mut domain, 2, b, 8, &mut sink),
            Err(Error::NotFound)
        );
    }

    #[test]
    fn remove_calls_the_source_remover_then_cascades_canonical_records() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let mut sink = NoSink;
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "abc",
                parent_native_id: None,
                name: "lamp",
            },
            &mut sink,
        )
        .unwrap();
        let entity: u64 = 7;
        let mut entity_record = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
        entity_record[..8].copy_from_slice(&id.to_le_bytes());
        domain
            .entity_put(
                ENTITY_ENTITY,
                &entity.to_le_bytes(),
                &entity_record,
                meta(2),
            )
            .unwrap();
        let state = ha_domain::canonical::StateKey {
            entity,
            property: 1,
        }
        .to_bytes();
        domain
            .entity_put(ENTITY_PROPERTY_STATE, &state, &[0u8; 8], meta(2))
            .unwrap();
        let mut group = [0u8; GROUP_ITEM_KEY_LEN];
        group[8..16].copy_from_slice(&entity.to_le_bytes());
        domain
            .entity_put(ENTITY_GROUP_ITEM, &group, &[0u8; 36], meta(2))
            .unwrap();

        let before = REMOVED.load(Ordering::Relaxed);
        assert!(remove(&mut domain, 2, id, &mut sink).unwrap() >= 2);
        assert_eq!(REMOVED.load(Ordering::Relaxed), before + 1);
        assert!(domain
            .entity_get(ENTITY_LOGICAL_DEVICE, &id.to_le_bytes(), &mut [0u8; 80])
            .is_err());
        assert!(domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut entity_record)
            .is_err());
        assert!(domain
            .entity_get(ENTITY_PROPERTY_STATE, &state, &mut [0u8; 8])
            .is_err());
        assert!(domain
            .entity_get(ENTITY_GROUP_ITEM, &group, &mut [0u8; 36])
            .is_err());
    }
}
