use std::sync::{Arc, Mutex};

use ha_domain::{
    Backing, Domain, DomainHandle, DomainMaintenance, EntityDesc, Error, FactMeta, StorageBackend,
    SharedStorage, StorageProvider, Value,
};
use micro_db::{
    FlashError, Geometry, Meta, NorFlash, RamFlashStorage, RamStorage, RegionManager, SimFlash,
    Storage, StorageMaintenance,
};

const ENTITY: u32 = 7;
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

    fn read(&self, offset: usize, out: &mut [u8]) -> Result<(), FlashError> {
        self.0.lock().unwrap().read(offset, out)
    }

    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashError> {
        self.0.lock().unwrap().write(offset, data)
    }

    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashError> {
        self.0.lock().unwrap().erase(offset, len)
    }
}

struct Provider {
    manager: RegionManager,
}

impl StorageProvider for Provider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        let geometry = Geometry {
            capacity: desc.capacity,
            key_size: desc.key_size,
            payload_size: desc.record_size,
        };
        let key = desc.persist_key.ok_or(Error::InvalidArg)?;
        let view = self
            .manager
            .bind(key, desc.capacity, desc.key_size, desc.record_size)?;
        Ok(StorageBackend::new(RamFlashStorage::open(
            view, geometry, key,
        )?))
    }
}

fn setup() -> (DomainHandle, DomainMaintenance) {
    let flash = SharedFlash::new();
    let mut domain = Domain::new_with_storage(8, 4, 16, || 0, Provider {
        manager: RegionManager::new(flash).unwrap(),
    })
    .unwrap();
    domain
        .register_entity(EntityDesc {
            entity: ENTITY,
            key_size: 8,
            record_size: 8,
            capacity: 8,
            backing: Backing::RamFlash,
            persist_key: Some("maintenance_test"),
        })
        .unwrap();
    domain.restore().unwrap();
    let maintenance = domain.take_maintenance().expect("maintenance handles");
    (DomainHandle::new(domain), maintenance)
}

#[test]
fn maintenance_compacts_without_changing_canonical_state() {
    let (domain, maintenance) = setup();
    let meta = FactMeta { source: 1, value: Value::NONE };
    domain.entity_put(ENTITY, &[1; 8], &[2; 8], meta).unwrap();

    maintenance.compact(ENTITY).unwrap();
    maintenance.sync(ENTITY).unwrap();
    assert!(maintenance.bank_state(ENTITY).unwrap().is_some());

    let mut record = [0; 8];
    domain.entity_get(ENTITY, &[1; 8], &mut record).unwrap();
    assert_eq!(record, [2; 8]);
}
#[derive(Default)]
struct TestState {
    inner: Option<RamStorage>,
    fail_write: bool,
    fail_compact: bool,
    bank: u32,
    write_enter: Option<Arc<std::sync::Barrier>>,
    write_release: Option<Arc<std::sync::Barrier>>,
    write_active: bool,
    compact_started: Option<std::sync::mpsc::SyncSender<()>>,
    compact_enter: Option<Arc<std::sync::Barrier>>,
    compact_release: Option<Arc<std::sync::Barrier>>,
    compact_saw_write: bool,
}

struct TestStorage {
    state: Arc<Mutex<TestState>>,
}

impl Storage for TestStorage {
    fn geometry(&self) -> Geometry {
        self.state.lock().unwrap().inner.as_ref().unwrap().geometry()
    }

    fn read_meta(&self, slot: u32) -> Result<Meta, Error> {
        self.state.lock().unwrap().inner.as_ref().unwrap().read_meta(slot)
    }

    fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<(), Error> {
        self.state.lock().unwrap().inner.as_ref().unwrap().read_key(slot, out)
    }

    fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta, Error> {
        self.state.lock().unwrap().inner.as_ref().unwrap().read_slot(slot, key, payload)
    }

    fn write_slot(&mut self, slot: u32, meta: Meta, key: &[u8], payload: &[u8]) -> Result<(), Error> {
        let (enter, release) = {
            let mut state = self.state.lock().unwrap();
            state.write_active = true;
            (state.write_enter.clone(), state.write_release.clone())
        };
        if let (Some(enter), Some(release)) = (enter, release) {
            enter.wait();
            release.wait();
        }
        let mut state = self.state.lock().unwrap();
        state.write_active = false;
        if state.fail_write {
            return Err(Error::NoSpace);
        }
        state.inner.as_mut().unwrap().write_slot(slot, meta, key, payload)
    }

    fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<(), Error> {
        let mut state = self.state.lock().unwrap();
        if state.fail_write {
            return Err(Error::NoSpace);
        }
        state.inner.as_mut().unwrap().write_meta(slot, meta)
    }

    fn clear_all(&mut self) -> Result<(), Error> {
        self.state.lock().unwrap().inner.as_mut().unwrap().clear_all()
    }

    fn sync(&mut self) -> Result<(), Error> {
        Ok(())
    }
}

impl StorageMaintenance for TestStorage {
    fn compact(&mut self) -> Result<(), Error> {
        let (enter, release) = {
            let state = self.state.lock().unwrap();
            (state.compact_enter.clone(), state.compact_release.clone())
        };
        if let (Some(enter), Some(release)) = (enter, release) {
            enter.wait();
            release.wait();
        }
        let mut state = self.state.lock().unwrap();
        if let Some(started) = state.compact_started.take() {
            let _ = started.send(());
        }
        if state.write_active {
            state.compact_saw_write = true;
            return Err(Error::InvalidState);
        }
        if state.fail_compact {
            return Err(Error::Io);
        }
        state.fail_write = false;
        state.bank += 1;
        Ok(())
    }

    fn bank_state(&self) -> Option<(usize, u32)> {
        Some((0, self.state.lock().unwrap().bank))
    }

    fn sync(&mut self) -> Result<(), Error> {
        Ok(())
    }
}

struct TestProvider {
    state: Arc<Mutex<TestState>>,
}

impl StorageProvider for TestProvider {
    fn open(&mut self, _desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        Ok(StorageBackend::new(TestStorage {
            state: Arc::clone(&self.state),
        }))
    }
}

fn test_setup() -> (DomainHandle, DomainMaintenance, Arc<Mutex<TestState>>, std::sync::mpsc::Receiver<ha_domain::Fact>) {
    let state = Arc::new(Mutex::new(TestState {
        inner: Some(RamStorage::new(Geometry { capacity: 4, key_size: 8, payload_size: 8 }).unwrap()),
        fail_write: false,
        fail_compact: false,
        bank: 0,
        write_enter: None,
        write_release: None,
        write_active: false,
        compact_started: None,
        compact_enter: None,
        compact_release: None,
        compact_saw_write: false,
    }));
    let mut domain = Domain::new_with_storage(8, 4, 16, || 0, TestProvider {
        state: Arc::clone(&state),
    })
    .unwrap();
    domain
        .register_entity(EntityDesc {
            entity: ENTITY,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::RamFlash,
            persist_key: Some("maintenance_test_2"),
        })
        .unwrap();
    let (tx, rx) = std::sync::mpsc::sync_channel(8);
    domain.subscribe_channel(ha_domain::Filter::all(), tx);
    domain.restore().unwrap();
    let maintenance = domain.take_maintenance().expect("maintenance");
    (DomainHandle::new(domain), maintenance, state, rx)
}

#[test]
fn no_space_requires_explicit_compaction_and_retry() {
    let (domain, maintenance, state, receiver) = test_setup();
    let meta = FactMeta { source: 1, value: Value::NONE };
    domain.entity_put(ENTITY, &[1; 8], &[2; 8], meta).unwrap();
    let _ = domain.dispatch_once();
    assert!(receiver.try_recv().is_ok());

    state.lock().unwrap().fail_write = true;
    assert_eq!(domain.entity_put(ENTITY, &[3; 8], &[4; 8], meta), Err(Error::NoSpace));
    assert_eq!(domain.entity_get(ENTITY, &[3; 8], &mut [0; 8]), Err(Error::NotFound));
    assert_eq!(domain.dispatch_once(), Ok(1));
    assert_eq!(
        receiver.try_recv().expect("put ERROR fact").kind,
        ha_domain::FactKind::Error
    );

    maintenance.compact(ENTITY).unwrap();
    domain.entity_put(ENTITY, &[3; 8], &[4; 8], meta).unwrap();
    let _ = domain.dispatch_once();
    assert!(receiver.try_recv().is_ok());
}

#[test]
fn failed_compaction_keeps_ram_and_bank_and_publishes_no_delta() {
    let (domain, maintenance, state, receiver) = test_setup();
    let meta = FactMeta { source: 1, value: Value::NONE };
    domain.entity_put(ENTITY, &[1; 8], &[2; 8], meta).unwrap();
    let _ = domain.dispatch_once();
    assert!(receiver.try_recv().is_ok());

    state.lock().unwrap().fail_write = true;
    assert_eq!(domain.entity_put(ENTITY, &[3; 8], &[4; 8], meta), Err(Error::NoSpace));
    state.lock().unwrap().fail_compact = true;
    assert_eq!(maintenance.compact(ENTITY), Err(Error::Io));
    assert_eq!(maintenance.bank_state(ENTITY), Ok(Some((0, 0))));
    assert_eq!(domain.entity_get(ENTITY, &[1; 8], &mut [0; 8]), Ok(()));
    assert_eq!(domain.dispatch_once(), Ok(1));
    assert_eq!(
        receiver.try_recv().expect("put ERROR fact").kind,
        ha_domain::FactKind::Error
    );
}
#[test]
fn mutation_and_compaction_share_the_table_gate() {
    let (_, _, state, _) = test_setup();
    let enter = Arc::new(std::sync::Barrier::new(2));
    let release = Arc::new(std::sync::Barrier::new(2));
    {
        let mut state = state.lock().unwrap();
        state.write_enter = Some(Arc::clone(&enter));
        state.write_release = Some(Arc::clone(&release));
    }

    let shared = SharedStorage::new(StorageBackend::new(TestStorage {
        state: Arc::clone(&state),
    }));
    let mut mutation_handle = shared.clone();
    let mutation = std::thread::spawn(move || {
        mutation_handle.write_slot(
            0,
            Meta {
                used: true,
                generation: 1,
                version: 1,
            },
            &[1; 8],
            &[2; 8],
        )
    });
    enter.wait();

    let (started_tx, started_rx) = std::sync::mpsc::sync_channel(1);
    state.lock().unwrap().compact_started = Some(started_tx);
    let maintenance_handle = shared.clone();
    let maintenance = std::thread::spawn(move || maintenance_handle.compact());

    assert_eq!(
        started_rx.recv_timeout(std::time::Duration::from_millis(200)),
        Err(std::sync::mpsc::RecvTimeoutError::Timeout)
    );
    release.wait();
    assert!(mutation.join().unwrap().is_ok());
    assert!(maintenance.join().unwrap().is_ok());
    assert!(!state.lock().unwrap().compact_saw_write);
}
struct TwoProvider {
    first: Arc<Mutex<TestState>>,
    second: Arc<Mutex<TestState>>,
}

impl StorageProvider for TwoProvider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, Error> {
        let state = if desc.entity == ENTITY {
            Arc::clone(&self.first)
        } else {
            Arc::clone(&self.second)
        };
        Ok(StorageBackend::new(TestStorage { state }))
    }
}

#[test]
fn maintenance_of_one_table_does_not_hold_domain_mutex_or_block_another_table() {
    let first = Arc::new(Mutex::new(TestState {
        inner: Some(RamStorage::new(Geometry { capacity: 4, key_size: 8, payload_size: 8 }).unwrap()),
        fail_write: false,
        fail_compact: false,
        bank: 0,
        write_enter: None,
        write_release: None,
        write_active: false,
        compact_started: None,
        compact_enter: None,
        compact_release: None,
        compact_saw_write: false,
    }));
    let second = Arc::new(Mutex::new(TestState {
        inner: Some(RamStorage::new(Geometry { capacity: 4, key_size: 8, payload_size: 8 }).unwrap()),
        fail_write: false,
        fail_compact: false,
        bank: 0,
        write_enter: None,
        write_release: None,
        write_active: false,
        compact_started: None,
        compact_enter: None,
        compact_release: None,
        compact_saw_write: false,
    }));
    let mut domain = Domain::new_with_storage(8, 4, 16, || 0, TwoProvider {
        first: Arc::clone(&first),
        second: Arc::clone(&second),
    })
    .unwrap();
    for entity in [ENTITY, ENTITY + 1] {
        domain
            .register_entity(EntityDesc {
                entity,
                key_size: 8,
                record_size: 8,
                capacity: 4,
                backing: Backing::RamFlash,
                persist_key: Some(if entity == ENTITY { "maintenance_a" } else { "maintenance_b" }),
            })
            .unwrap();
    }
    domain.restore().unwrap();
    let maintenance = domain.take_maintenance().expect("maintenance");
    let handle = DomainHandle::new(domain);

    let enter = Arc::new(std::sync::Barrier::new(2));
    let release = Arc::new(std::sync::Barrier::new(2));
    {
        let mut state = first.lock().unwrap();
        state.compact_enter = Some(Arc::clone(&enter));
        state.compact_release = Some(Arc::clone(&release));
    }
    let maintenance_thread = std::thread::spawn(move || maintenance.compact(ENTITY));
    enter.wait();

    let (tx, rx) = std::sync::mpsc::sync_channel(1);
    let mutation_handle = handle.clone();
    let mutation = std::thread::spawn(move || {
        let result = mutation_handle.entity_put(
            ENTITY + 1,
            &[1; 8],
            &[2; 8],
            FactMeta { source: 1, value: Value::NONE },
        );
        tx.send(result).unwrap();
    });
    assert!(rx.recv_timeout(std::time::Duration::from_millis(200)).is_ok());
    release.wait();
    assert!(maintenance_thread.join().unwrap().is_ok());
    mutation.join().unwrap();
}