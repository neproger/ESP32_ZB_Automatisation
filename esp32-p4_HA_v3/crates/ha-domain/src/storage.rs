use std::sync::{Arc, Mutex, MutexGuard};

use crate::{EntityType, Error};

use crate::entity::EntityDesc;

use micro_db::{Geometry, Meta, Result, Storage, StorageMaintenance};

pub trait StorageBackendImpl: Storage + StorageMaintenance + Send {}

impl<T: Storage + StorageMaintenance + Send> StorageBackendImpl for T {}

/// Type-erased storage so `Domain` can hold RAM and RAM+Flash tables together.
pub struct StorageBackend(Box<dyn StorageBackendImpl>);

impl StorageBackend {
    pub fn new<S: StorageBackendImpl + 'static>(storage: S) -> Self {
        Self(Box::new(storage))
    }
}

impl StorageMaintenance for StorageBackend {
    fn compact(&mut self) -> Result<()> {
        StorageMaintenance::compact(&mut *self.0)
    }

    fn bank_state(&self) -> Option<(usize, u32)> {
        StorageMaintenance::bank_state(&*self.0)
    }

    fn sync(&mut self) -> Result<()> {
        StorageMaintenance::sync(&mut *self.0)
    }
}

impl Storage for StorageBackend {
    fn geometry(&self) -> Geometry {
        self.0.geometry()
    }

    fn read_meta(&self, slot: u32) -> Result<Meta> {
        self.0.read_meta(slot)
    }

    fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<()> {
        self.0.read_key(slot, out)
    }

    fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta> {
        self.0.read_slot(slot, key, payload)
    }

    fn write_slot(&mut self, slot: u32, meta: Meta, key: &[u8], payload: &[u8]) -> Result<()> {
        self.0.write_slot(slot, meta, key, payload)
    }

    fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<()> {
        self.0.write_meta(slot, meta)
    }

    fn clear_all(&mut self) -> Result<()> {
        self.0.clear_all()
    }

    fn sync(&mut self) -> Result<()> {
        Storage::sync(&mut *self.0)
    }
}

#[derive(Clone)]
pub struct SharedStorage(Arc<Mutex<StorageBackend>>);

impl SharedStorage {
    pub fn new(storage: StorageBackend) -> Self {
        Self(Arc::new(Mutex::new(storage)))
    }

    fn lock(&self) -> Result<MutexGuard<'_, StorageBackend>> {
        self.0.lock().map_err(|_| Error::InvalidState)
    }

    pub fn compact(&self) -> Result<()> {
        let mut storage = self.lock()?;
        StorageMaintenance::compact(&mut *storage)
    }

    pub fn bank_state(&self) -> Result<Option<(usize, u32)>> {
        Ok(StorageMaintenance::bank_state(&*self.lock()?))
    }

    pub fn sync(&self) -> Result<()> {
        let mut storage = self.lock()?;
        StorageMaintenance::sync(&mut *storage)
    }
}

impl Storage for SharedStorage {
    fn geometry(&self) -> Geometry {
        self.lock().expect("shared storage geometry").geometry()
    }

    fn read_meta(&self, slot: u32) -> Result<Meta> {
        self.lock()?.read_meta(slot)
    }

    fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<()> {
        self.lock()?.read_key(slot, out)
    }

    fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta> {
        self.lock()?.read_slot(slot, key, payload)
    }

    fn write_slot(&mut self, slot: u32, meta: Meta, key: &[u8], payload: &[u8]) -> Result<()> {
        self.lock()?.write_slot(slot, meta, key, payload)
    }

    fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<()> {
        self.lock()?.write_meta(slot, meta)
    }

    fn clear_all(&mut self) -> Result<()> {
        self.lock()?.clear_all()
    }

    fn sync(&mut self) -> Result<()> {
        let mut storage = self.lock()?;
        Storage::sync(&mut *storage)
    }
}

impl StorageMaintenance for SharedStorage {
    fn compact(&mut self) -> Result<()> {
        SharedStorage::compact(self)
    }

    fn bank_state(&self) -> Option<(usize, u32)> {
        SharedStorage::bank_state(self).ok().flatten()
    }

    fn sync(&mut self) -> Result<()> {
        SharedStorage::sync(self)
    }
}

pub struct DomainMaintenance {
    handles: Vec<(EntityType, SharedStorage)>,
}

impl DomainMaintenance {
    pub(crate) fn new(handles: Vec<(EntityType, SharedStorage)>) -> Self {
        Self { handles }
    }

    fn storage(&self, entity: EntityType) -> Result<&SharedStorage> {
        self.handles
            .iter()
            .find(|(candidate, _)| *candidate == entity)
            .map(|(_, storage)| storage)
            .ok_or(Error::NotFound)
    }

    pub fn compact(&self, entity: EntityType) -> Result<()> {
        self.storage(entity)?.compact()
    }

    pub fn bank_state(&self, entity: EntityType) -> Result<Option<(usize, u32)>> {
        self.storage(entity)?.bank_state()
    }

    pub fn sync(&self, entity: EntityType) -> Result<()> {
        self.storage(entity)?.sync()
    }
}

/// Opens one storage backend per descriptor. Implemented by the composition root.
/// Must remain usable after an `open` error so a failed restore can be retried;
/// failed opens may leave directory entries that must be reusable.
pub trait StorageProvider: Send {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend>;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Backing {
    Ram,
    Flash,
    RamFlash,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RestorePhase {
    StorageOpen,
    TableOpen,
    Lifecycle,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RestoreError {
    pub entity: EntityType,
    pub phase: RestorePhase,
    pub error: Error,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EntityRestore {
    pub entity: EntityType,
    pub records: usize,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RestoreReport {
    pub entities: Vec<EntityRestore>,
}
