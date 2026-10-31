use micro_db::{Geometry, Meta, Result, Storage};

use crate::EntityDesc;

/// Type-erased storage so `Domain` can hold RAM and RAM+Flash tables together.
pub struct StorageBackend(Box<dyn Storage + Send>);

impl StorageBackend {
    pub fn new<S: Storage + Send + 'static>(storage: S) -> Self {
        Self(Box::new(storage))
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
        self.0.sync()
    }
}

/// Opens one storage backend per descriptor. Implemented by the composition root.
/// Must remain usable after an `open` error so a failed restore can be retried;
/// failed opens may leave directory entries that must be reusable.
pub trait StorageProvider: Send {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend>;
}
