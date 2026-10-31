use std::sync::{Arc, Mutex};

use ha_domain::canonical::{
    StateKey, AUTOMATION_KEY_LEN, AUTOMATION_RECORD_LEN, ENTITY_AUTOMATION, ENTITY_GROUP,
    ENTITY_PROPERTY_STATE, GROUP_KEY_LEN, GROUP_RECORD_LEN, STATE_KEY_LEN, STATE_RECORD_LEN,
};
use ha_domain::{
    Backing, Domain, EntityDesc, EntityType, Error, FactMeta, RestorePhase, StorageBackend,
    StorageProvider, Value,
};
use micro_db::{
    FlashError, Geometry, Meta, NorFlash, RamFlashStorage, RamStorage, RegionManager, SimFlash,
    Storage,
};

const RAM_ENTITY: EntityType = 3;
const ERASE: usize = 4096;
const FLASH_SIZE: usize = 32768;

#[derive(Clone)]
struct SharedFlash(Arc<Mutex<SimFlash>>);

impl SharedFlash {
    fn new() -> Self {
        Self(Arc::new(Mutex::new(SimFlash::new(FLASH_SIZE, ERASE))))
    }
}

impl NorFlash for SharedFlash {
    fn capacity(&self) -> usize {
        self.0.lock().unwrap().capacity()
    }

    fn erase_size(&self) -> usize {
        self.0.lock().unwrap().erase_size()
    }

    fn read(&self, offset: usize, out: &mut [u8]) -> core::result::Result<(), FlashError> {
        self.0.lock().unwrap().read(offset, out)
    }

    fn write(&mut self, offset: usize, data: &[u8]) -> core::result::Result<(), FlashError> {
        self.0.lock().unwrap().write(offset, data)
    }

    fn erase(&mut self, offset: usize, len: usize) -> core::result::Result<(), FlashError> {
        self.0.lock().unwrap().erase(offset, len)
    }
}

struct RegionProvider {
    manager: RegionManager,
}

impl RegionProvider {
    fn new(flash: SharedFlash) -> Result<Self, Error> {
        Ok(Self {
            manager: RegionManager::new(flash)?,
        })
    }
}

impl StorageProvider for RegionProvider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        let geometry = Geometry {
            capacity: desc.capacity,
            key_size: desc.key_size,
            payload_size: desc.record_size,
        };
        match desc.backing {
            Backing::Ram => Ok(StorageBackend::new(RamStorage::new(geometry)?)),
            Backing::RamFlash => {
                let key = desc.persist_key.ok_or(Error::InvalidArg)?;
                let view =
                    self.manager
                        .bind(key, desc.capacity, desc.key_size, desc.record_size)?;
                let storage = RamFlashStorage::open(view, geometry, key)?;
                Ok(StorageBackend::new(storage))
            }
            Backing::Flash => Err(Error::InvalidState),
        }
    }
}

struct FaultProvider {
    inner: RegionProvider,
    fail: Arc<Mutex<bool>>,
    target: EntityType,
}

impl StorageProvider for FaultProvider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        let mut fail = self.fail.lock().unwrap();
        if *fail && desc.entity == self.target {
            *fail = false;
            return Err(Error::Io);
        }
        drop(fail);
        self.inner.open(desc)
    }
}

struct WriteFailStorage {
    inner: RamStorage,
    fail: Arc<Mutex<bool>>,
}

impl Storage for WriteFailStorage {
    fn geometry(&self) -> Geometry {
        self.inner.geometry()
    }

    fn read_meta(&self, slot: u32) -> Result<Meta, Error> {
        self.inner.read_meta(slot)
    }

    fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<(), Error> {
        self.inner.read_key(slot, out)
    }

    fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta, Error> {
        self.inner.read_slot(slot, key, payload)
    }

    fn write_slot(
        &mut self,
        slot: u32,
        meta: Meta,
        key: &[u8],
        payload: &[u8],
    ) -> Result<(), Error> {
        if *self.fail.lock().unwrap() {
            return Err(Error::Io);
        }
        self.inner.write_slot(slot, meta, key, payload)
    }

    fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<(), Error> {
        if *self.fail.lock().unwrap() {
            return Err(Error::Io);
        }
        self.inner.write_meta(slot, meta)
    }

    fn clear_all(&mut self) -> Result<(), Error> {
        self.inner.clear_all()
    }

    fn sync(&mut self) -> Result<(), Error> {
        self.inner.sync()
    }
}

struct WriteFailProvider {
    fail: Arc<Mutex<bool>>,
}

impl StorageProvider for WriteFailProvider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        let geometry = Geometry {
            capacity: desc.capacity,
            key_size: desc.key_size,
            payload_size: desc.record_size,
        };
        Ok(StorageBackend::new(WriteFailStorage {
            inner: RamStorage::new(geometry)?,
            fail: Arc::clone(&self.fail),
        }))
    }
}

fn manifest() -> Vec<EntityDesc<'static>> {
    vec![
        EntityDesc {
            entity: ENTITY_AUTOMATION,
            key_size: AUTOMATION_KEY_LEN,
            record_size: AUTOMATION_RECORD_LEN,
            capacity: 4,
            backing: Backing::RamFlash,
            persist_key: Some("automation_v5"),
        },
        EntityDesc {
            entity: ENTITY_GROUP,
            key_size: GROUP_KEY_LEN,
            record_size: GROUP_RECORD_LEN,
            capacity: 4,
            backing: Backing::RamFlash,
            persist_key: Some("group"),
        },
        EntityDesc {
            entity: ENTITY_PROPERTY_STATE,
            key_size: STATE_KEY_LEN,
            record_size: STATE_RECORD_LEN,
            capacity: 8,
            backing: Backing::Ram,
            persist_key: None,
        },
    ]
}

fn clock() -> u64 {
    1
}

fn meta() -> FactMeta {
    FactMeta {
        source: 1,
        value: Value::NONE,
    }
}

fn register_manifest(domain: &mut Domain) {
    for desc in manifest() {
        domain.register_entity(desc).unwrap();
    }
}

fn new_domain(provider: impl StorageProvider + 'static) -> Domain {
    Domain::new_with_storage(8, 4, 16, clock, provider).unwrap()
}

fn record_key(entity: EntityType) -> [u8; 8] {
    let mut key = [0u8; 8];
    key[0] = entity as u8;
    key
}

#[test]
fn manifest_uses_canonical_sizes_and_v2_policy() {
    let manifest = manifest();
    assert_eq!(manifest[0].key_size, AUTOMATION_KEY_LEN);
    assert_eq!(manifest[0].record_size, AUTOMATION_RECORD_LEN);
    assert_eq!(manifest[0].backing, Backing::RamFlash);
    assert_eq!(manifest[0].persist_key, Some("automation_v5"));
    assert_eq!(manifest[1].key_size, GROUP_KEY_LEN);
    assert_eq!(manifest[1].record_size, GROUP_RECORD_LEN);
    assert_eq!(manifest[1].persist_key, Some("group"));
    assert_eq!(manifest[2].backing, Backing::Ram);
    assert_eq!(manifest[2].key_size, STATE_KEY_LEN);
    assert_eq!(manifest[2].record_size, STATE_RECORD_LEN);
}

#[test]
fn restore_is_atomic_and_preserves_flash_records_and_versions() {
    let flash = SharedFlash::new();
    let mut domain = new_domain(RegionProvider::new(flash.clone()).unwrap());
    register_manifest(&mut domain);
    let report = domain.restore().unwrap();
    assert_eq!(report.entities.len(), 3);
    assert!(report.entities.iter().all(|entity| entity.records == 0));

    let key = record_key(ENTITY_AUTOMATION);
    let record = [0xA5u8; AUTOMATION_RECORD_LEN];
    assert!(domain
        .entity_put(ENTITY_AUTOMATION, &key, &record, meta())
        .unwrap());
    let version = domain.entity_meta(ENTITY_AUTOMATION, &key).unwrap().version;

    let state_key = StateKey {
        entity: 1,
        property: 1,
    }
    .to_bytes();
    domain
        .entity_put(
            ENTITY_PROPERTY_STATE,
            &state_key,
            &[0x11u8; STATE_RECORD_LEN],
            meta(),
        )
        .unwrap();
    drop(domain);

    let mut reopened = new_domain(RegionProvider::new(flash.clone()).unwrap());
    register_manifest(&mut reopened);
    let report = reopened.restore().unwrap();
    let automation = report
        .entities
        .iter()
        .find(|entity| entity.entity == ENTITY_AUTOMATION)
        .unwrap();
    assert_eq!(automation.records, 1);
    let group = report
        .entities
        .iter()
        .find(|entity| entity.entity == ENTITY_GROUP)
        .unwrap();
    assert_eq!(group.records, 0);

    let mut out = [0u8; AUTOMATION_RECORD_LEN];
    reopened
        .entity_get(ENTITY_AUTOMATION, &key, &mut out)
        .unwrap();
    assert_eq!(out, record);
    assert_eq!(
        reopened
            .entity_meta(ENTITY_AUTOMATION, &key)
            .unwrap()
            .version,
        version
    );

    let mut state = [0u8; STATE_RECORD_LEN];
    assert_eq!(
        reopened.entity_get(ENTITY_PROPERTY_STATE, &state_key, &mut state),
        Err(Error::NotFound)
    );
}

#[test]
fn restore_failure_is_atomic_and_retryable() {
    let flash = SharedFlash::new();
    let fail = Arc::new(Mutex::new(true));
    let provider = FaultProvider {
        inner: RegionProvider::new(flash).unwrap(),
        fail: Arc::clone(&fail),
        target: ENTITY_GROUP,
    };
    let mut domain = new_domain(provider);
    register_manifest(&mut domain);

    let error = domain.restore().unwrap_err();
    assert_eq!(error.entity, ENTITY_GROUP);
    assert_eq!(error.phase, RestorePhase::StorageOpen);
    assert_eq!(error.error, Error::Io);

    let mut out = [0u8; AUTOMATION_RECORD_LEN];
    assert_eq!(
        domain.entity_get(ENTITY_AUTOMATION, &record_key(ENTITY_AUTOMATION), &mut out),
        Err(Error::InvalidState)
    );

    assert_eq!(*fail.lock().unwrap(), false);
    let report = domain.restore().unwrap();
    assert_eq!(report.entities.len(), 3);
}

#[test]
fn restore_does_not_publish_facts() {
    let flash = SharedFlash::new();
    let mut first = new_domain(RegionProvider::new(flash.clone()).unwrap());
    register_manifest(&mut first);
    first.restore().unwrap();
    first
        .entity_put(
            ENTITY_AUTOMATION,
            &record_key(ENTITY_AUTOMATION),
            &[0x22u8; AUTOMATION_RECORD_LEN],
            meta(),
        )
        .unwrap();
    drop(first);

    let mut reopened = new_domain(RegionProvider::new(flash).unwrap());
    register_manifest(&mut reopened);
    let seen = Arc::new(Mutex::new(0usize));
    let sink = Arc::clone(&seen);
    reopened.subscribe(ha_domain::Filter::all(), move |_| {
        *sink.lock().unwrap() += 1;
        true
    });
    reopened.restore().unwrap();
    assert_eq!(reopened.dispatch_once(), 0);
    assert_eq!(*seen.lock().unwrap(), 0);
}

#[test]
fn storage_write_error_returns_err_without_fact() {
    let fail = Arc::new(Mutex::new(false));
    let mut domain = new_domain(WriteFailProvider {
        fail: Arc::clone(&fail),
    });
    domain
        .register_entity(EntityDesc {
            entity: RAM_ENTITY,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    domain.restore().unwrap();

    let seen = Arc::new(Mutex::new(0usize));
    let sink = Arc::clone(&seen);
    domain.subscribe(ha_domain::Filter::all(), move |_| {
        *sink.lock().unwrap() += 1;
        true
    });
    *fail.lock().unwrap() = true;
    assert_eq!(
        domain.entity_put(RAM_ENTITY, &[1u8; 8], &[2u8; 8], meta()),
        Err(Error::Io)
    );
    let mut out = [0u8; 8];
    assert_eq!(
        domain.entity_get(RAM_ENTITY, &[1u8; 8], &mut out),
        Err(Error::NotFound)
    );
    assert_eq!(domain.dispatch_once(), 0);
    assert_eq!(*seen.lock().unwrap(), 0);

    *fail.lock().unwrap() = false;
    domain
        .entity_put(RAM_ENTITY, &[1u8; 8], &[2u8; 8], meta())
        .unwrap();
    let version = domain.entity_meta(RAM_ENTITY, &[1u8; 8]).unwrap().version;
    domain.entity_get(RAM_ENTITY, &[1u8; 8], &mut out).unwrap();
    assert_eq!(out, [2u8; 8]);
    domain.dispatch_once();
    *fail.lock().unwrap() = true;
    assert_eq!(
        domain.entity_remove(RAM_ENTITY, &[1u8; 8], meta()),
        Err(Error::Io)
    );
    domain.entity_get(RAM_ENTITY, &[1u8; 8], &mut out).unwrap();
    assert_eq!(out, [2u8; 8]);
    assert_eq!(
        domain.entity_meta(RAM_ENTITY, &[1u8; 8]).unwrap().version,
        version
    );
    assert_eq!(domain.dispatch_once(), 0);
    assert_eq!(*seen.lock().unwrap(), 1);
}

struct TableFailProvider {
    inner: RegionProvider,
    fail: Arc<Mutex<bool>>,
    target: EntityType,
}

impl StorageProvider for TableFailProvider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        let mut fail = self.fail.lock().unwrap();
        if *fail && desc.entity == self.target {
            *fail = false;
            drop(fail);
            let key = desc.persist_key.ok_or(Error::InvalidArg)?;
            let view =
                self.inner
                    .manager
                    .bind(key, desc.capacity, desc.key_size, desc.record_size)?;
            drop(view);
            let wrong = Geometry {
                capacity: desc.capacity,
                key_size: desc.key_size,
                payload_size: desc.record_size + 1,
            };
            return Ok(StorageBackend::new(RamStorage::new(wrong)?));
        }
        drop(fail);
        self.inner.open(desc)
    }
}

#[test]
fn restore_table_open_error_releases_region_and_retries() {
    let flash = SharedFlash::new();
    let fail = Arc::new(Mutex::new(true));
    let provider = TableFailProvider {
        inner: RegionProvider::new(flash).unwrap(),
        fail: Arc::clone(&fail),
        target: ENTITY_GROUP,
    };
    let mut domain = new_domain(provider);
    register_manifest(&mut domain);

    let error = domain.restore().unwrap_err();
    assert_eq!(error.entity, ENTITY_GROUP);
    assert_eq!(error.phase, RestorePhase::TableOpen);
    assert_eq!(error.error, Error::InvalidState);
    assert_eq!(*fail.lock().unwrap(), false);

    let report = domain.restore().unwrap();
    assert_eq!(report.entities.len(), 3);
}

struct WrongGeometryProvider;

impl StorageProvider for WrongGeometryProvider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        let geometry = Geometry {
            capacity: desc.capacity,
            key_size: desc.key_size,
            payload_size: desc.record_size + 1,
        };
        Ok(StorageBackend::new(RamStorage::new(geometry)?))
    }
}

#[test]
fn restore_reports_table_open_phase() {
    let mut domain = new_domain(WrongGeometryProvider);
    domain
        .register_entity(EntityDesc {
            entity: RAM_ENTITY,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    let error = domain.restore().unwrap_err();
    assert_eq!(error.entity, RAM_ENTITY);
    assert_eq!(error.phase, RestorePhase::TableOpen);
    assert_eq!(error.error, Error::InvalidState);
}

#[test]
fn registration_rules_enforce_backing_and_persist_key() {
    let flash = SharedFlash::new();
    let mut domain = new_domain(RegionProvider::new(flash).unwrap());
    register_manifest(&mut domain);

    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: 99,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::RamFlash,
            persist_key: Some("automation_v5"),
        }),
        Err(Error::AlreadyExists)
    );
    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: 100,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::RamFlash,
            persist_key: None,
        }),
        Err(Error::InvalidArg)
    );
    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: 103,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: Some("group"),
        }),
        Err(Error::AlreadyExists)
    );
    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: 101,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::Flash,
            persist_key: Some("flash"),
        }),
        Err(Error::InvalidState)
    );
    domain.restore().unwrap();
    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: 102,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: None,
        }),
        Err(Error::InvalidState)
    );
    assert_eq!(domain.restore().unwrap_err().phase, RestorePhase::Lifecycle);
}
