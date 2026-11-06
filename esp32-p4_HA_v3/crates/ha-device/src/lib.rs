#![forbid(unsafe_code)]

//! Neutral Device service: canonical Logical Device lifecycle and ownership.
//!
//! It knows only opaque source/native ids and canonical records; the adapter owns
//! the physical representation and registers a remover for its source.
//!
//! `ensure_entity` and `set_capabilities` are Rust/Z2M adaptations: V2 creates
//! Entity/capabilities in the adapter, while here the Device service owns the
//! canonical write. They are not V2 Device service endpoints.

use std::sync::Mutex;

use ha_domain::canonical::{
    device_id_derive, CapabilitiesRecord, DeviceId, EntityId, EntityRecord, LogicalDeviceRecord,
    SourceId, DEVICE_RECORD_LEN, ENTITY_CAPABILITIES, ENTITY_ENTITY, ENTITY_GROUP_ITEM,
    ENTITY_LOGICAL_DEVICE, ENTITY_PROPERTY_STATE, ENTITY_RECORD_LEN, GROUP_ITEM_KEY_LEN,
    STATE_KEY_LEN,
};
use ha_domain::Error;
use ha_domain::{Domain, DomainHandle, DomainTransaction, FactMeta, Value};

/// Minimal canonical entity access used by Device operations.
///
/// Implemented by `Domain` and `DomainTransaction`, so Device service logic
/// can run either directly or inside one Domain critical section.
pub trait CanonicalStore {
    fn entity_get(&mut self, entity: u32, key: &[u8], out: &mut [u8]) -> Result<(), Error>;
    fn entity_put(
        &mut self,
        entity: u32,
        key: &[u8],
        record: &[u8],
        meta: FactMeta,
    ) -> Result<bool, Error>;
}

impl CanonicalStore for Domain {
    fn entity_get(&mut self, entity: u32, key: &[u8], out: &mut [u8]) -> Result<(), Error> {
        Domain::entity_get(self, entity, key, out)
    }

    fn entity_put(
        &mut self,
        entity: u32,
        key: &[u8],
        record: &[u8],
        meta: FactMeta,
    ) -> Result<bool, Error> {
        Domain::entity_put(self, entity, key, record, meta)
    }
}

impl CanonicalStore for DomainTransaction<'_> {
    fn entity_get(&mut self, entity: u32, key: &[u8], out: &mut [u8]) -> Result<(), Error> {
        DomainTransaction::entity_get(self, entity, key, out)
    }

    fn entity_put(
        &mut self,
        entity: u32,
        key: &[u8],
        record: &[u8],
        meta: FactMeta,
    ) -> Result<bool, Error> {
        DomainTransaction::entity_put(self, entity, key, record, meta)
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NativeRemoval {
    Removed,
    AlreadyAbsent,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RemovalError {
    Remover(Error),
    Canonical(Error),
    Progress(Error),
    StalePlan,
}

impl From<Error> for RemovalError {
    fn from(error: Error) -> Self {
        Self::Canonical(error)
    }
}

pub type Operation = fn(&[u8]) -> Result<NativeRemoval, Error>;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemovalRecord {
    pub entity: u32,
    pub key: Vec<u8>,
    pub version: u32,
}

#[derive(Debug, PartialEq, Eq)]
pub struct RemovalPlan {
    device: DeviceId,
    source: SourceId,
    native_id: Vec<u8>,
    device_version: u32,
    records: Vec<RemovalRecord>,
}

#[derive(Debug, PartialEq, Eq)]
struct RemovalProgress {
    bits: Vec<u8>,
}

#[derive(Debug, PartialEq, Eq)]
pub struct RemovalTicket {
    plan: RemovalPlan,
    native: NativeRemoval,
    progress: RemovalProgress,
}

impl RemovalProgress {
    fn new(records: usize) -> Result<Self, Error> {
        let bytes = records.checked_add(7).ok_or(Error::Overflow)? / 8;
        let mut bits = Vec::new();
        bits.try_reserve_exact(bytes).map_err(|_| Error::NoMemory)?;
        bits.resize(bytes, 0);
        Ok(Self { bits })
    }

    fn is_done(&self, index: usize) -> bool {
        self.bits
            .get(index / 8)
            .is_some_and(|byte| byte & (1 << (index % 8)) != 0)
    }

    fn mark(&mut self, index: usize) {
        if let Some(byte) = self.bits.get_mut(index / 8) {
            *byte |= 1 << (index % 8);
        }
    }
}

const REMOVERS_MAX: usize = 4;
const REMOVAL_RECORDS_MAX: usize = 1024;

struct Remover {
    source: SourceId,
    operation: Operation,
}

static REMOVERS: Mutex<Vec<Remover>> = Mutex::new(Vec::new());

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

pub fn register<S: CanonicalStore>(
    domain: &mut S,
    origin: u8,
    desc: &DeviceDesc<'_>,
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
            domain.entity_put(ENTITY_LOGICAL_DEVICE, &key, &encoded, meta(origin))?;
        }
        return Ok(id);
    }

    let record = LogicalDeviceRecord::new(desc.source, parent, desc.native_id, desc.name);
    let encoded = record.encode();
    domain.entity_put(ENTITY_LOGICAL_DEVICE, &key, &encoded, meta(origin))?;
    Ok(id)
}

pub fn ensure_entity<S: CanonicalStore>(
    domain: &mut S,
    origin: u8,
    device: DeviceId,
    entity: EntityId,
    name: &str,
) -> Result<bool, Error> {
    if device == 0 || entity == 0 {
        return Err(Error::InvalidArg);
    }
    let key = entity.to_le_bytes();
    let mut current = [0u8; ENTITY_RECORD_LEN];
    match domain.entity_get(ENTITY_ENTITY, &key, &mut current) {
        Ok(()) => {
            let owner = u64::from_le_bytes(current[..8].try_into().expect("entity owner"));
            if owner != 0 && owner != device {
                return Err(Error::InvalidState);
            }
            Ok(false)
        }
        Err(Error::NotFound) => {
            let record = EntityRecord::new(device, name);
            let encoded = record.encode();
            domain.entity_put(ENTITY_ENTITY, &key, &encoded, meta(origin))
        }
        Err(error) => Err(error),
    }
}

pub fn set_capabilities<S: CanonicalStore>(
    domain: &mut S,
    origin: u8,
    entity: EntityId,
    capabilities: &CapabilitiesRecord,
) -> Result<bool, Error> {
    if entity == 0 {
        return Err(Error::InvalidArg);
    }
    let mut current = [0u8; ENTITY_RECORD_LEN];
    domain.entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut current)?;
    let owner = u64::from_le_bytes(current[..8].try_into().expect("entity owner"));
    if owner == 0 {
        return Err(Error::InvalidState);
    }
    let encoded = capabilities.encode();
    domain.entity_put(
        ENTITY_CAPABILITIES,
        &entity.to_le_bytes(),
        &encoded,
        meta(origin),
    )
}

pub fn bind_entity<S: CanonicalStore>(
    domain: &mut S,
    origin: u8,
    device: DeviceId,
    entity: EntityId,
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
    domain.entity_put(ENTITY_ENTITY, &key, &record, meta(origin))
}

pub fn rename<S: CanonicalStore>(
    domain: &mut S,
    origin: u8,
    device: DeviceId,
    name: &str,
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
    domain.entity_put(ENTITY_LOGICAL_DEVICE, &key, &encoded, meta(origin))
}

fn push_record(
    transaction: &mut DomainTransaction<'_>,
    entity: u32,
    key: &[u8],
    records: &mut Vec<RemovalRecord>,
) -> Result<(), RemovalError> {
    if records.len() == REMOVAL_RECORDS_MAX {
        return Err(RemovalError::Canonical(Error::NoSpace));
    }
    let version = transaction.entity_meta(entity, key)?.version;
    records.push(RemovalRecord {
        entity,
        key: key.to_vec(),
        version,
    });
    Ok(())
}

pub fn prepare_remove(
    domain: &DomainHandle,
    device: DeviceId,
) -> Result<RemovalPlan, RemovalError> {
    if device == 0 {
        return Err(RemovalError::Canonical(Error::InvalidArg));
    }
    domain.transaction::<RemovalPlan, RemovalError>(|transaction| {
        let key = device.to_le_bytes();
        let mut current = [0u8; DEVICE_RECORD_LEN];
        transaction.entity_get(ENTITY_LOGICAL_DEVICE, &key, &mut current)?;
        let record = decode_device(&current);
        let device_version = transaction
            .entity_meta(ENTITY_LOGICAL_DEVICE, &key)?
            .version;
        let native_len = record
            .native_id
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(record.native_id.len());
        let native_id = record.native_id[..native_len].to_vec();

        let entity_snapshot = transaction.entity_snapshot(ENTITY_ENTITY)?;
        let state_snapshot = transaction.entity_snapshot(ENTITY_PROPERTY_STATE)?;
        let group_item_snapshot = transaction.entity_snapshot(ENTITY_GROUP_ITEM)?;

        let mut entities = Vec::new();
        for index in 0..entity_snapshot.len() {
            let key = entity_snapshot.key(index).expect("entity snapshot key");
            let value = entity_snapshot
                .record(index)
                .expect("entity snapshot record");
            if key.len() == 8
                && value.len() >= 8
                && u64::from_le_bytes(value[..8].try_into().expect("entity owner")) == device
            {
                if entities.len() == REMOVAL_RECORDS_MAX {
                    return Err(RemovalError::Canonical(Error::NoSpace));
                }
                entities.push(u64::from_le_bytes(key.try_into().expect("entity key")));
            }
        }
        entities.sort_unstable();

        let mut records = Vec::new();
        for entity in entities {
            let entity_bytes = entity.to_le_bytes();
            let mut states = Vec::new();
            let mut group_items = Vec::new();
            for index in 0..state_snapshot.len() {
                let key = state_snapshot.key(index).expect("state snapshot key");
                if key.len() == STATE_KEY_LEN && key[..8] == entity_bytes {
                    if states.len() == REMOVAL_RECORDS_MAX {
                        return Err(RemovalError::Canonical(Error::NoSpace));
                    }
                    states.push(key.to_vec());
                }
            }
            for index in 0..group_item_snapshot.len() {
                let key = group_item_snapshot
                    .key(index)
                    .expect("group item snapshot key");
                if key.len() == GROUP_ITEM_KEY_LEN && key[8..16] == entity_bytes {
                    if group_items.len() == REMOVAL_RECORDS_MAX {
                        return Err(RemovalError::Canonical(Error::NoSpace));
                    }
                    group_items.push(key.to_vec());
                }
            }
            states.sort();
            group_items.sort();
            for state in states {
                push_record(transaction, ENTITY_PROPERTY_STATE, &state, &mut records)?;
            }
            for item in group_items {
                push_record(transaction, ENTITY_GROUP_ITEM, &item, &mut records)?;
            }
            match transaction.entity_meta(ENTITY_CAPABILITIES, &entity_bytes) {
                Ok(_) => {
                    push_record(
                        transaction,
                        ENTITY_CAPABILITIES,
                        &entity_bytes,
                        &mut records,
                    )?;
                }
                Err(Error::NotFound) => {}
                Err(error) => return Err(error.into()),
            }
            push_record(transaction, ENTITY_ENTITY, &entity_bytes, &mut records)?;
        }
        push_record(transaction, ENTITY_LOGICAL_DEVICE, &key, &mut records)?;

        Ok(RemovalPlan {
            device,
            source: record.source,
            native_id,
            device_version,
            records,
        })
    })
}
fn plan_has(plan: &RemovalPlan, entity: u32, key: &[u8]) -> bool {
    plan.records
        .iter()
        .any(|record| record.entity == entity && record.key == key)
}

fn plan_owned_entity(plan: &RemovalPlan, entity: EntityId) -> bool {
    plan.records.iter().any(|record| {
        record.entity == ENTITY_ENTITY
            && record.key.len() == 8
            && u64::from_le_bytes(record.key[..8].try_into().expect("entity key")) == entity
    })
}

fn validate_plan(
    transaction: &mut DomainTransaction<'_>,
    plan: &RemovalPlan,
    progress: &RemovalProgress,
) -> Result<(), RemovalError> {
    for (index, planned) in plan.records.iter().enumerate() {
        let done = progress.is_done(index);
        match transaction.entity_meta(planned.entity, &planned.key) {
            Ok(meta) => {
                if done
                    || meta.version != planned.version
                    || (planned.entity == ENTITY_LOGICAL_DEVICE
                        && meta.version != plan.device_version)
                {
                    return Err(RemovalError::StalePlan);
                }
                if planned.entity == ENTITY_ENTITY {
                    let mut record = [0u8; ENTITY_RECORD_LEN];
                    transaction.entity_get(ENTITY_ENTITY, &planned.key, &mut record)?;
                    let owner = u64::from_le_bytes(record[..8].try_into().expect("entity owner"));
                    if owner != plan.device {
                        return Err(RemovalError::StalePlan);
                    }
                } else if planned.entity == ENTITY_LOGICAL_DEVICE {
                    let mut record = [0u8; DEVICE_RECORD_LEN];
                    transaction.entity_get(ENTITY_LOGICAL_DEVICE, &planned.key, &mut record)?;
                    let current = decode_device(&record);
                    let native_len = current
                        .native_id
                        .iter()
                        .position(|byte| *byte == 0)
                        .unwrap_or(current.native_id.len());
                    if current.source != plan.source
                        || current.native_id[..native_len] != plan.native_id[..]
                    {
                        return Err(RemovalError::StalePlan);
                    }
                }
            }
            Err(Error::NotFound) if done => {}
            Err(Error::NotFound) => return Err(RemovalError::StalePlan),
            Err(error) => return Err(error.into()),
        }
    }

    let mut extra = false;
    let entity_snapshot = transaction.entity_snapshot(ENTITY_ENTITY)?;
    for index in 0..entity_snapshot.len() {
        let key = entity_snapshot.key(index).expect("entity snapshot key");
        let value = entity_snapshot
            .record(index)
            .expect("entity snapshot record");
        if key.len() == 8
            && value.len() >= 8
            && u64::from_le_bytes(value[..8].try_into().expect("entity owner")) == plan.device
        {
            let entity = u64::from_le_bytes(key.try_into().expect("entity key"));
            if !plan_has(plan, ENTITY_ENTITY, &entity.to_le_bytes()) {
                extra = true;
            }
        }
    }
    let capability_snapshot = transaction.entity_snapshot(ENTITY_CAPABILITIES)?;
    for index in 0..capability_snapshot.len() {
        let key = capability_snapshot
            .key(index)
            .expect("capability snapshot key");
        if key.len() == 8 {
            let entity = u64::from_le_bytes(key.try_into().expect("cap entity"));
            if plan_owned_entity(plan, entity) && !plan_has(plan, ENTITY_CAPABILITIES, key) {
                extra = true;
            }
        }
    }
    let state_snapshot = transaction.entity_snapshot(ENTITY_PROPERTY_STATE)?;
    for index in 0..state_snapshot.len() {
        let key = state_snapshot.key(index).expect("state snapshot key");
        if key.len() >= 8 {
            let entity = u64::from_le_bytes(key[..8].try_into().expect("state entity"));
            if plan_owned_entity(plan, entity) && !plan_has(plan, ENTITY_PROPERTY_STATE, key) {
                extra = true;
            }
        }
    }
    let group_item_snapshot = transaction.entity_snapshot(ENTITY_GROUP_ITEM)?;
    for index in 0..group_item_snapshot.len() {
        let key = group_item_snapshot
            .key(index)
            .expect("group item snapshot key");
        if key.len() == GROUP_ITEM_KEY_LEN {
            let entity = u64::from_le_bytes(key[8..16].try_into().expect("group item entity"));
            if plan_owned_entity(plan, entity) && !plan_has(plan, ENTITY_GROUP_ITEM, key) {
                extra = true;
            }
        }
    }
    if extra {
        return Err(RemovalError::StalePlan);
    }
    Ok(())
}

pub fn invoke_remover(plan: RemovalPlan) -> Result<RemovalTicket, RemovalError> {
    let progress = RemovalProgress::new(plan.records.len()).map_err(RemovalError::Progress)?;
    let operation = remover(plan.source).ok_or(RemovalError::Remover(Error::InvalidState))?;
    let native = operation(&plan.native_id).map_err(RemovalError::Remover)?;
    Ok(RemovalTicket {
        plan,
        native,
        progress,
    })
}

pub fn remove_canonical(
    domain: &DomainHandle,
    origin: u8,
    ticket: &mut RemovalTicket,
) -> Result<usize, RemovalError> {
    domain.transaction::<usize, RemovalError>(|transaction| {
        validate_plan(transaction, &ticket.plan, &ticket.progress)?;
        let mut removed = 0;
        for (index, record) in ticket.plan.records.iter().enumerate() {
            if ticket.progress.is_done(index) {
                continue;
            }
            transaction.entity_remove(record.entity, &record.key, meta(origin))?;
            ticket.progress.mark(index);
            removed += 1;
        }
        Ok(removed)
    })
}

impl RemovalTicket {
    pub fn native_removal(&self) -> NativeRemoval {
        self.native
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::{Backing, EntityDesc};
    use std::sync::atomic::{AtomicUsize, Ordering};

    static REMOVED: AtomicUsize = AtomicUsize::new(0);

    fn remover(_native: &[u8]) -> Result<NativeRemoval, Error> {
        REMOVED.fetch_add(1, Ordering::Relaxed);
        Ok(NativeRemoval::Removed)
    }

    fn absent_remover(_native: &[u8]) -> Result<NativeRemoval, Error> {
        Ok(NativeRemoval::AlreadyAbsent)
    }

    fn failing_remover(_native: &[u8]) -> Result<NativeRemoval, Error> {
        Err(Error::InvalidState)
    }

    fn handle(domain: Domain) -> DomainHandle {
        DomainHandle::new(domain)
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
        let desc = DeviceDesc {
            source: 1,
            native_id: "abc",
            parent_native_id: None,
            name: "lamp",
        };
        let id = register(&mut domain, 2, &desc).unwrap();
        let renamed = DeviceDesc {
            name: "other",
            ..desc
        };
        assert_eq!(register(&mut domain, 2, &renamed), Ok(id));
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
        let a = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "a",
                parent_native_id: None,
                name: "",
            },
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
        )
        .unwrap();
        let entity: u64 = 7;
        let mut record = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
        domain
            .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &record, meta(2))
            .unwrap();
        assert_eq!(bind_entity(&mut domain, 2, a, entity), Ok(true));
        assert_eq!(bind_entity(&mut domain, 2, a, entity), Ok(false));
        assert_eq!(
            bind_entity(&mut domain, 2, b, entity),
            Err(Error::InvalidState)
        );
        record[..8].copy_from_slice(&a.to_le_bytes());
        assert_eq!(bind_entity(&mut domain, 2, b, 8), Err(Error::NotFound));
    }

    fn add_entity(domain: &mut Domain, device: DeviceId, entity: EntityId) {
        let mut record = [0u8; ENTITY_RECORD_LEN];
        record[..8].copy_from_slice(&device.to_le_bytes());
        domain
            .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &record, meta(2))
            .unwrap();
        let caps = CapabilitiesRecord::default().encode();
        domain
            .entity_put(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &caps, meta(2))
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
    }

    fn add_entity_handle(domain: &DomainHandle, device: DeviceId, entity: EntityId) {
        let mut record = [0u8; ENTITY_RECORD_LEN];
        record[..8].copy_from_slice(&device.to_le_bytes());
        domain
            .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &record, meta(2))
            .unwrap();
        let caps = CapabilitiesRecord::default().encode();
        domain
            .entity_put(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &caps, meta(2))
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
    }

    #[test]
    fn ensure_entity_and_capabilities_are_device_owned() {
        let mut domain = domain();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "ensure",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        let entity = 7;
        assert!(ensure_entity(&mut domain, 2, id, entity, "lamp").unwrap());
        assert!(!ensure_entity(&mut domain, 2, id, entity, "other").unwrap());
        let caps = CapabilitiesRecord::default();
        assert!(set_capabilities(&mut domain, 2, entity, &caps).unwrap());
        assert!(domain
            .entity_get(
                ENTITY_CAPABILITIES,
                &entity.to_le_bytes(),
                &mut [0u8; ha_domain::canonical::CAPABILITIES_LEN]
            )
            .is_ok());
    }

    #[test]
    fn ensure_entity_rejects_foreign_owner_without_mutation() {
        let mut domain = domain();
        let owner = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "owner",
                parent_native_id: None,
                name: "",
            },
        )
        .unwrap();
        let other = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "other",
                parent_native_id: None,
                name: "",
            },
        )
        .unwrap();
        let entity: u64 = 7;
        let mut record = [0u8; ENTITY_RECORD_LEN];
        record[..8].copy_from_slice(&owner.to_le_bytes());
        domain
            .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &record, meta(2))
            .unwrap();

        assert_eq!(
            ensure_entity(&mut domain, 2, other, entity, "replacement"),
            Err(Error::InvalidState)
        );
        let mut current = [0u8; ENTITY_RECORD_LEN];
        domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut current)
            .unwrap();
        assert_eq!(
            u64::from_le_bytes(current[..8].try_into().expect("entity owner")),
            owner
        );
        assert_eq!(&current[8..], &record[8..]);
    }

    #[test]
    fn set_capabilities_requires_an_existing_owned_entity() {
        let mut domain = domain();
        let caps = CapabilitiesRecord::default();

        assert_eq!(
            set_capabilities(&mut domain, 2, 7, &caps),
            Err(Error::NotFound)
        );

        let mut record = [0u8; ENTITY_RECORD_LEN];
        domain
            .entity_put(ENTITY_ENTITY, &7u64.to_le_bytes(), &record, meta(2))
            .unwrap();
        assert_eq!(
            set_capabilities(&mut domain, 2, 7, &caps),
            Err(Error::InvalidState)
        );

        record[..8].copy_from_slice(&9u64.to_le_bytes());
        domain
            .entity_put(ENTITY_ENTITY, &7u64.to_le_bytes(), &record, meta(2))
            .unwrap();
        assert!(set_capabilities(&mut domain, 2, 7, &caps).unwrap());
    }

    #[test]
    fn remove_calls_the_source_remover_then_cascades_canonical_records() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "abc",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        let entity: u64 = 7;
        add_entity(&mut domain, id, entity);
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();
        let mut ticket = invoke_remover(plan).unwrap();
        assert!(remove_canonical(&domain, 2, &mut ticket).unwrap() >= 5);
        assert!(domain
            .entity_get(ENTITY_LOGICAL_DEVICE, &id.to_le_bytes(), &mut [0u8; 80])
            .is_err());
        assert!(domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut [0u8; 36])
            .is_err());
        assert!(remove_canonical(&domain, 2, &mut ticket).unwrap() == 0);
    }

    #[test]
    fn stale_plan_does_not_delete_changed_or_new_records() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "stale",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        let entity = 7;
        add_entity(&mut domain, id, entity);
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();
        let mut ticket = invoke_remover(plan).unwrap();

        let mut record = [0u8; ENTITY_RECORD_LEN];
        domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut record)
            .unwrap();
        record[8] = 1;
        domain
            .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &record, meta(2))
            .unwrap();
        assert_eq!(
            remove_canonical(&domain, 2, &mut ticket),
            Err(RemovalError::StalePlan)
        );
        assert!(domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut record)
            .is_ok());
    }

    #[test]
    fn changed_ownership_after_plan_is_stale() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "owner-change",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        let entity = 7;
        add_entity(&mut domain, id, entity);
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();
        let mut ticket = invoke_remover(plan).unwrap();

        let mut record = [0u8; ENTITY_RECORD_LEN];
        domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut record)
            .unwrap();
        record[..8].copy_from_slice(&8u64.to_le_bytes());
        domain
            .entity_put(ENTITY_ENTITY, &entity.to_le_bytes(), &record, meta(2))
            .unwrap();
        assert_eq!(
            remove_canonical(&domain, 2, &mut ticket),
            Err(RemovalError::StalePlan)
        );
        assert!(domain
            .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut record)
            .is_ok());
    }

    #[test]
    fn completed_progress_is_skipped_and_ticket_retry_is_safe() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "progress",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        let entity = 7;
        add_entity(&mut domain, id, entity);
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();
        let mut ticket = invoke_remover(plan).unwrap();
        let first = ticket.plan.records[0].clone();
        domain
            .entity_remove(first.entity, &first.key, meta(2))
            .unwrap();
        ticket.progress.mark(0);
        assert!(remove_canonical(&domain, 2, &mut ticket).unwrap() >= 4);
        assert!(remove_canonical(&domain, 2, &mut ticket).unwrap() == 0);
    }

    #[test]
    fn failed_removal_does_not_mark_progress() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "failed-progress",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        add_entity(&mut domain, id, 7);
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();
        let mut ticket = invoke_remover(plan).unwrap();

        assert_eq!(
            remove_canonical(&domain, 32, &mut ticket),
            Err(RemovalError::Canonical(Error::InvalidArg))
        );
        assert!(!ticket.progress.is_done(0));
        assert!(domain
            .entity_get(
                ENTITY_ENTITY,
                &7u64.to_le_bytes(),
                &mut [0u8; ENTITY_RECORD_LEN]
            )
            .is_ok());
    }

    #[test]
    fn missing_without_progress_or_reappearing_record_is_stale() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "missing",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        let entity = 7;
        add_entity(&mut domain, id, entity);
        let domain = handle(domain);

        let plan = prepare_remove(&domain, id).unwrap();
        let mut missing_ticket = invoke_remover(plan).unwrap();
        let first = missing_ticket.plan.records[0].clone();
        domain
            .entity_remove(first.entity, &first.key, meta(2))
            .unwrap();
        assert_eq!(
            remove_canonical(&domain, 2, &mut missing_ticket),
            Err(RemovalError::StalePlan)
        );

        let plan = prepare_remove(&domain, id).unwrap();
        let mut reappearing_ticket = invoke_remover(plan).unwrap();
        reappearing_ticket.progress.mark(0);
        assert_eq!(
            remove_canonical(&domain, 2, &mut reappearing_ticket),
            Err(RemovalError::StalePlan)
        );
    }

    #[test]
    fn new_owned_entity_after_plan_is_stale() {
        let mut domain = domain();
        register_remover(1, remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "new-entity",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        add_entity(&mut domain, id, 7);
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();
        let mut ticket = invoke_remover(plan).unwrap();
        add_entity_handle(&domain, id, 8);
        assert_eq!(
            remove_canonical(&domain, 2, &mut ticket),
            Err(RemovalError::StalePlan)
        );
        assert!(domain
            .entity_get(
                ENTITY_ENTITY,
                &7u64.to_le_bytes(),
                &mut [0u8; ENTITY_RECORD_LEN]
            )
            .is_ok());
    }

    #[test]
    fn prepare_remove_snapshot_error_does_not_mutate_domain() {
        let mut domain = Domain::new(16, 4, 32).unwrap();
        domain
            .register_entity(EntityDesc {
                entity: ENTITY_LOGICAL_DEVICE,
                key_size: 8,
                record_size: DEVICE_RECORD_LEN,
                capacity: 1,
                backing: Backing::Ram,
                persist_key: None,
            })
            .unwrap();
        domain
            .register_entity(EntityDesc {
                entity: ENTITY_ENTITY,
                key_size: 8,
                record_size: ENTITY_RECORD_LEN,
                capacity: 512,
                backing: Backing::Ram,
                persist_key: None,
            })
            .unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 1,
                native_id: "oversized",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        for index in 0..410u64 {
            let mut key = [0u8; 8];
            key.copy_from_slice(&index.to_le_bytes());
            domain
                .entity_put(ENTITY_ENTITY, &key, &[0u8; ENTITY_RECORD_LEN], meta(2))
                .unwrap();
        }
        let domain = handle(domain);
        assert_eq!(
            prepare_remove(&domain, id),
            Err(RemovalError::Canonical(Error::InvalidSize))
        );
        assert!(domain
            .entity_get(
                ENTITY_LOGICAL_DEVICE,
                &id.to_le_bytes(),
                &mut [0u8; DEVICE_RECORD_LEN]
            )
            .is_ok());
        assert!(domain
            .entity_get(ENTITY_ENTITY, &[0u8; 8], &mut [0u8; ENTITY_RECORD_LEN])
            .is_ok());
    }

    #[test]
    fn failed_remover_leaves_canonical_state_untouched() {
        let mut domain = domain();
        register_remover(3, failing_remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 3,
                native_id: "failing-remover",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        add_entity(&mut domain, id, 7);
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();

        assert_eq!(
            invoke_remover(plan),
            Err(RemovalError::Remover(Error::InvalidState))
        );
        assert!(domain
            .entity_get(
                ENTITY_LOGICAL_DEVICE,
                &id.to_le_bytes(),
                &mut [0u8; DEVICE_RECORD_LEN]
            )
            .is_ok());
        assert!(domain
            .entity_get(
                ENTITY_ENTITY,
                &7u64.to_le_bytes(),
                &mut [0u8; ENTITY_RECORD_LEN]
            )
            .is_ok());
    }

    #[test]
    fn already_absent_remover_is_successful() {
        let mut domain = domain();
        register_remover(2, absent_remover).unwrap();
        let id = register(
            &mut domain,
            2,
            &DeviceDesc {
                source: 2,
                native_id: "absent",
                parent_native_id: None,
                name: "lamp",
            },
        )
        .unwrap();
        let domain = handle(domain);
        let plan = prepare_remove(&domain, id).unwrap();
        let mut ticket = invoke_remover(plan).unwrap();
        assert_eq!(ticket.native_removal(), NativeRemoval::AlreadyAbsent);
        assert!(remove_canonical(&domain, 2, &mut ticket).unwrap() >= 1);
    }
}
