use core::hash::Hasher;

use crate::flash::{Geometry, NorFlash};
use crate::storage::{FlashStorage, RamFlashStorage, Storage};

/// Operation outcome. Names match V2 `SYS_CODE_*` used by mstore.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    NotFound,
    Stale,
    AlreadyExists,
    NoSpace,
    InvalidArg,
    InvalidState,
    InvalidSize,
    NoMemory,
    InvariantFailed,
    Overflow,
    Io,
    Corrupt,
}

pub type Result<T> = core::result::Result<T, Error>;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Meta {
    pub used: bool,
    pub generation: u32,
    pub version: u32,
}

/// Safe cached reference to one slot revision: valid while the slot is used and
/// its generation is unchanged. `version` is the snapshot taken with the handle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Handle {
    pub slot: u32,
    pub generation: u32,
    pub version: u32,
}

/// Cheap metadata check result: no key or payload is read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HandleStatus {
    Removed,
    Stale,
    Unchanged,
    Changed(Meta),
}

#[derive(Default)]
struct Fnv1a(u64);

impl Hasher for Fnv1a {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        let mut hash = if self.0 == 0 {
            0xcbf29ce484222325
        } else {
            self.0
        };
        for byte in bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
        self.0 = hash;
    }
}

const KEY_PROBE_MAX: usize = 64;
const NO_BUCKET: usize = usize::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ByteIndexEntry {
    slot: u32,
    home: usize,
}

fn index_buckets(capacity: usize) -> Result<usize> {
    capacity
        .checked_mul(2)
        .and_then(|value| value.checked_next_power_of_two())
        .ok_or(Error::InvalidSize)
}

/// Byte-oriented table engine shared by every storage backend.
///
/// The engine owns slot identity and derived state (index, free-list, live
/// count); `Storage` owns the canonical slot bytes. `home` in the index is the
/// key's original bucket, not the insert position.
pub struct Table<S: Storage> {
    geometry: Geometry,
    storage: S,
    index: Vec<Option<ByteIndexEntry>>,
    index_of_slot: Vec<usize>,
    free: Vec<u32>,
    live: usize,
    key_scratch: Vec<u8>,
    payload_scratch: Vec<u8>,
}

impl<S: Storage> Table<S> {
    pub fn open(geometry: Geometry, storage: S) -> Result<Self> {
        if geometry.capacity == 0 || geometry.key_size == 0 {
            return Err(Error::InvalidSize);
        }
        if geometry.key_size > KEY_PROBE_MAX || geometry.capacity > u32::MAX as usize {
            return Err(Error::InvalidSize);
        }
        if storage.geometry() != geometry {
            return Err(Error::InvalidState);
        }
        let buckets = index_buckets(geometry.capacity)?;
        let mut table = Self {
            geometry,
            storage,
            index: vec![None; buckets],
            index_of_slot: vec![NO_BUCKET; geometry.capacity],
            free: Vec::with_capacity(geometry.capacity),
            live: 0,
            key_scratch: vec![0; geometry.key_size],
            payload_scratch: vec![0; geometry.payload_size],
        };
        table.rebuild()?;
        Ok(table)
    }

    pub fn capacity(&self) -> usize {
        self.geometry.capacity
    }

    pub fn len(&self) -> usize {
        self.live
    }

    pub fn is_empty(&self) -> bool {
        self.live == 0
    }

    pub fn storage(&self) -> &S {
        &self.storage
    }

    pub fn into_storage(self) -> S {
        self.storage
    }

    pub fn find(&self, key: &[u8]) -> Result<u32> {
        if key.len() != self.geometry.key_size {
            return Err(Error::InvalidSize);
        }
        let mut probe = [0u8; KEY_PROBE_MAX];
        let mut bucket = self.bucket(key);
        loop {
            let Some(entry) = self.index[bucket] else {
                return Err(Error::NotFound);
            };
            self.storage
                .read_key(entry.slot, &mut probe[..self.geometry.key_size])?;
            if &probe[..self.geometry.key_size] == key {
                return Ok(entry.slot);
            }
            bucket = self.next(bucket);
        }
    }

    pub fn handle(&self, key: &[u8]) -> Result<Handle> {
        let slot = self.find(key)?;
        let meta = self.meta(slot)?;
        Ok(Handle {
            slot,
            generation: meta.generation,
            version: meta.version,
        })
    }

    /// V2 cheap metadata check: one `read_meta`, no key or payload reads.
    pub fn check(&self, handle: Handle) -> Result<HandleStatus> {
        self.check_slot(handle.slot)?;
        let meta = self.storage.read_meta(handle.slot)?;
        if meta.generation != handle.generation {
            return Ok(HandleStatus::Stale);
        }
        if !meta.used {
            return Ok(HandleStatus::Removed);
        }
        if meta.version == handle.version {
            Ok(HandleStatus::Unchanged)
        } else {
            Ok(HandleStatus::Changed(meta))
        }
    }

    pub fn meta(&self, slot: u32) -> Result<Meta> {
        self.check_slot(slot)?;
        let meta = self.storage.read_meta(slot)?;
        if meta.used {
            Ok(meta)
        } else {
            Err(Error::Stale)
        }
    }

    pub fn read(&self, slot: u32, key_out: &mut [u8], payload_out: &mut [u8]) -> Result<Meta> {
        self.check_slot(slot)?;
        if key_out.len() < self.geometry.key_size
            || payload_out.len() < self.geometry.payload_size
        {
            return Err(Error::InvalidSize);
        }
        let meta = self.storage.read_meta(slot)?;
        if !meta.used {
            return Err(Error::Stale);
        }
        self.storage.read_slot(slot, key_out, payload_out)
    }

    pub fn get(&self, key: &[u8], payload_out: &mut [u8]) -> Result<Meta> {
        if payload_out.len() < self.geometry.payload_size {
            return Err(Error::InvalidSize);
        }
        let slot = self.find(key)?;
        let mut key_out = [0u8; KEY_PROBE_MAX];
        self.read(slot, &mut key_out, payload_out)
    }

    pub fn allocate(&mut self, key: &[u8], payload: &[u8]) -> Result<Handle> {
        if key.len() != self.geometry.key_size || payload.len() != self.geometry.payload_size {
            return Err(Error::InvalidSize);
        }
        match self.find(key) {
            Ok(_) => return Err(Error::AlreadyExists),
            Err(Error::NotFound) => {}
            Err(error) => return Err(error),
        }
        if self.live == self.geometry.capacity {
            return Err(Error::NoSpace);
        }

        let mut picked = None;
        for position in (0..self.free.len()).rev() {
            let slot = self.free[position];
            let meta = self.storage.read_meta(slot)?;
            if meta.generation != u32::MAX {
                picked = Some(position);
                break;
            }
        }
        let position = picked.ok_or(Error::Overflow)?;
        let slot = self.free[position];
        let mut meta = self.storage.read_meta(slot)?;
        meta.used = true;
        meta.generation += 1;
        meta.version = 1;
        self.storage.write_slot(slot, meta, key, payload)?;

        self.free.swap_remove(position);
        self.index_insert(key, slot);
        self.live += 1;
        Ok(Handle {
            slot,
            generation: meta.generation,
            version: meta.version,
        })
    }

    pub fn update(&mut self, handle: Handle, payload: &[u8]) -> Result<bool> {
        if payload.len() != self.geometry.payload_size {
            return Err(Error::InvalidSize);
        }
        let meta = self.storage.read_meta(handle.slot)?;
        if !meta.used || meta.generation != handle.generation {
            return Err(Error::Stale);
        }
        let _ = self.storage.read_slot(
            handle.slot,
            &mut self.key_scratch,
            &mut self.payload_scratch,
        )?;
        if self.payload_scratch.as_slice() == payload {
            return Ok(false);
        }
        if meta.version == u32::MAX {
            return Err(Error::Overflow);
        }
        let mut next = meta;
        next.version += 1;
        self.storage
            .write_slot(handle.slot, next, &self.key_scratch, payload)?;
        Ok(true)
    }

    pub fn free(&mut self, handle: Handle) -> Result<()> {
        let meta = self.storage.read_meta(handle.slot)?;
        if !meta.used || meta.generation != handle.generation {
            return Err(Error::Stale);
        }
        let mut tombstone = meta;
        tombstone.used = false;
        self.storage.write_meta(handle.slot, tombstone)?;

        self.index_remove(handle.slot);
        self.free.push(handle.slot);
        self.live -= 1;
        Ok(())
    }

    pub fn iter(&self, mut visit: impl FnMut(u32, Meta, &[u8], &[u8])) -> Result<()> {
        let mut key = vec![0u8; self.geometry.key_size];
        let mut payload = vec![0u8; self.geometry.payload_size];
        for slot in 0..self.geometry.capacity as u32 {
            let meta = self.storage.read_meta(slot)?;
            if !meta.used {
                continue;
            }
            let meta = self.storage.read_slot(slot, &mut key, &mut payload)?;
            visit(slot, meta, &key, &payload);
        }
        Ok(())
    }

    pub fn clear(&mut self) -> Result<()> {
        self.storage.clear_all()?;
        self.reset_empty();
        Ok(())
    }

    fn reset_empty(&mut self) {
        self.index.fill(None);
        self.index_of_slot.fill(NO_BUCKET);
        self.free.clear();
        self.free.extend(0..self.geometry.capacity as u32);
        self.live = 0;
    }

    fn check_slot(&self, slot: u32) -> Result<()> {
        if slot as usize >= self.geometry.capacity {
            return Err(Error::InvalidArg);
        }
        Ok(())
    }

    fn bucket(&self, key: &[u8]) -> usize {
        let mut hasher = Fnv1a::default();
        hasher.write(key);
        (hasher.finish() as usize) & (self.index.len() - 1)
    }

    fn next(&self, bucket: usize) -> usize {
        (bucket + 1) & (self.index.len() - 1)
    }

    fn index_insert(&mut self, key: &[u8], slot: u32) {
        let home = self.bucket(key);
        self.index_insert_at(home, slot);
    }

    fn index_insert_at(&mut self, home: usize, slot: u32) {
        let mut bucket = home;
        while self.index[bucket].is_some() {
            bucket = self.next(bucket);
        }
        self.index[bucket] = Some(ByteIndexEntry { slot, home });
        self.index_of_slot[slot as usize] = bucket;
    }

    fn index_remove(&mut self, slot: u32) {
        let mut hole = self.index_of_slot[slot as usize];
        if hole == NO_BUCKET {
            return;
        }
        self.index_of_slot[slot as usize] = NO_BUCKET;
        self.index[hole] = None;

        let mut scan = self.next(hole);
        while let Some(entry) = self.index[scan] {
            let home = entry.home;
            let stays = if hole <= scan {
                hole < home && home <= scan
            } else {
                hole < home || home <= scan
            };
            if stays {
                scan = self.next(scan);
                continue;
            }
            self.index[hole] = Some(entry);
            self.index_of_slot[entry.slot as usize] = hole;
            self.index[scan] = None;
            hole = scan;
            scan = self.next(scan);
        }
    }

    fn rebuild(&mut self) -> Result<()> {
        self.index.fill(None);
        self.index_of_slot.fill(NO_BUCKET);
        self.free.clear();
        self.live = 0;

        for slot in 0..self.geometry.capacity as u32 {
            let meta = self.storage.read_meta(slot)?;
            if !meta.used {
                self.free.push(slot);
                continue;
            }
            self.storage.read_key(slot, &mut self.key_scratch)?;
            match self.find(&self.key_scratch) {
                Ok(_) => return Err(Error::Corrupt),
                Err(Error::NotFound) => {}
                Err(error) => return Err(error),
            }
            let home = self.bucket(&self.key_scratch);
            self.index_insert_at(home, slot);
            self.live += 1;
        }
        Ok(())
    }
}

impl<F: NorFlash> Table<FlashStorage<F>> {
    pub fn compact(&mut self) -> Result<()> {
        self.storage.compact()
    }

    pub fn bank_state(&self) -> (usize, u32) {
        self.storage.bank_state()
    }
}

impl<F: NorFlash> Table<RamFlashStorage<F>> {
    pub fn compact(&mut self) -> Result<()> {
        self.storage.compact()
    }

    pub fn bank_state(&self) -> (usize, u32) {
        self.storage.bank_state()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::flash::FlashError;
    use crate::ring::Ring;
    use crate::{RamFlashStorage, RamStorage, SimFlash, Storage};
    use std::cell::Cell;
    use std::rc::Rc;

    #[test]
    fn ring_ordering_eviction_and_bounds() {
        let mut ring = Ring::new(3).unwrap();
        assert_eq!(ring.oldest_seq(), Err(Error::NotFound));
        assert_eq!(ring.newest_seq(), Err(Error::NotFound));
        assert_eq!(ring.append(10), Ok(1));
        assert_eq!(ring.append(11), Ok(2));
        assert_eq!(ring.append(12), Ok(3));
        assert_eq!(ring.count(), 3);
        assert_eq!(ring.oldest_seq(), Ok(1));
        assert_eq!(ring.newest_seq(), Ok(3));
        assert!(ring.contains(2).unwrap());
        assert_eq!(ring.append(13), Ok(4));
        assert_eq!(ring.count(), 3);
        assert_eq!(ring.oldest_seq(), Ok(2));
        assert!(!ring.contains(1).unwrap());
        assert_eq!(ring.get(1), Err(Error::Stale));
        assert_eq!(ring.get(4), Ok(13));
        assert_eq!(ring.get(5), Err(Error::NotFound));
        assert_eq!(ring.get(0), Err(Error::NotFound));
    }

    fn byte_geometry() -> Geometry {
        Geometry {
            capacity: 8,
            key_size: 8,
            payload_size: 12,
        }
    }

    fn byte_key(id: u8) -> [u8; 8] {
        [id; 8]
    }

    fn byte_payload(id: u8) -> [u8; 12] {
        [id; 12]
    }

    fn byte_table() -> Table<RamStorage> {
        let geometry = byte_geometry();
        Table::open(geometry, RamStorage::new(geometry).unwrap()).unwrap()
    }

    #[test]
    fn table_requires_matching_storage_geometry() {
        let geometry = byte_geometry();

        let mut capacity = geometry;
        capacity.capacity = 3;
        assert_eq!(
            Table::open(geometry, RamStorage::new(capacity).unwrap()).err(),
            Some(Error::InvalidState)
        );

        let mut key = geometry;
        key.key_size = 4;
        assert_eq!(
            Table::open(geometry, RamStorage::new(key).unwrap()).err(),
            Some(Error::InvalidState)
        );

        let mut payload = geometry;
        payload.payload_size = 4;
        assert_eq!(
            Table::open(geometry, RamStorage::new(payload).unwrap()).err(),
            Some(Error::InvalidState)
        );
    }

    #[test]
    fn table_allocate_update_free() {
        let mut table = byte_table();
        let handle = table
            .allocate(&byte_key(1), &byte_payload(1))
            .unwrap();
        assert_eq!(table.find(&byte_key(1)), Ok(handle.slot));
        assert_eq!(table.meta(handle.slot).unwrap().generation, handle.generation);
        assert!(!table.update(handle, &byte_payload(1)).unwrap());
        assert!(table.update(handle, &byte_payload(2)).unwrap());

        let mut out = [0u8; 12];
        let meta = table.get(&byte_key(1), &mut out).unwrap();
        assert_eq!(meta.version, 2);
        assert_eq!(out, byte_payload(2));

        table.free(handle).unwrap();
        assert_eq!(table.find(&byte_key(1)), Err(Error::NotFound));
        assert_eq!(table.meta(handle.slot), Err(Error::Stale));
        assert_eq!(table.len(), 0);
    }

    #[test]
    fn table_collisions_and_backward_shift() {
        let mut table = byte_table();
        let mut groups: std::collections::HashMap<usize, Vec<[u8; 8]>> =
            std::collections::HashMap::new();
        for id in 0..=255u8 {
            let key = byte_key(id);
            groups.entry(table.bucket(&key)).or_default().push(key);
        }
        let keys: Vec<[u8; 8]> = groups
            .values()
            .find(|group| group.len() >= 3)
            .expect("a bucket with at least three keys")
            .iter()
            .take(3)
            .copied()
            .collect();

        let handles: Vec<Handle> = keys
            .iter()
            .enumerate()
            .map(|(index, key)| table.allocate(key, &byte_payload(index as u8)).unwrap())
            .collect();

        for (removed, slot) in [0usize, 1, 2].into_iter().enumerate() {
            table.free(handles[slot]).unwrap();
            for (index, key) in keys.iter().enumerate() {
                if index > removed {
                    assert!(table.find(key).is_ok(), "key {index} lost");
                }
            }
        }

        table.rebuild().unwrap();
        for key in &keys {
            assert_eq!(table.find(key), Err(Error::NotFound));
        }
        table.allocate(&keys[1], &byte_payload(9)).unwrap();
        assert!(table.find(&keys[1]).is_ok());
        assert_eq!(table.len(), 1);
    }

    #[test]
    fn table_allocate_skips_exhausted_generation() {
        let geometry = Geometry {
            capacity: 2,
            key_size: 8,
            payload_size: 0,
        };
        let mut storage = RamStorage::new(geometry).unwrap();
        storage
            .write_meta(
                0,
                Meta {
                    used: false,
                    generation: u32::MAX,
                    version: 0,
                },
            )
            .unwrap();
        storage
            .write_meta(
                1,
                Meta {
                    used: false,
                    generation: 0,
                    version: 0,
                },
            )
            .unwrap();

        let mut table = Table::open(geometry, storage).unwrap();
        let handle = table.allocate(&byte_key(1), &[]).unwrap();
        assert_eq!(handle.slot, 1);
        assert_eq!(
            table.allocate(&byte_key(2), &[]),
            Err(Error::Overflow)
        );
    }

    #[test]
    fn table_rebuild_rejects_duplicate_keys() {
        let geometry = byte_geometry();
        let mut storage = RamStorage::new(geometry).unwrap();
        let meta = Meta {
            used: true,
            generation: 1,
            version: 1,
        };
        storage
            .write_slot(0, meta, &byte_key(1), &byte_payload(1))
            .unwrap();
        storage
            .write_slot(1, meta, &byte_key(1), &byte_payload(1))
            .unwrap();
        assert_eq!(
            Table::open(geometry, storage).err(),
            Some(Error::Corrupt)
        );
    }

    #[test]
    fn index_buckets_rejects_overflow() {
        assert_eq!(
            index_buckets(usize::MAX / 2 + 1),
            Err(Error::InvalidSize)
        );
    }

    #[test]
    fn table_clear_preserves_generation() {
        let geometry = Geometry {
            capacity: 1,
            key_size: 8,
            payload_size: 0,
        };
        let mut table =
            Table::open(geometry, RamStorage::new(geometry).unwrap()).unwrap();
        let first = table.allocate(&byte_key(1), &[]).unwrap();
        assert_eq!(first.generation, 1);
        table.clear().unwrap();
        assert_eq!(table.len(), 0);
        let second = table.allocate(&byte_key(1), &[]).unwrap();
        assert_eq!(second.generation, 2);
    }

    struct FaultyStorage {
        inner: RamStorage,
        geometry: Geometry,
        fail_read_key: bool,
        fail_read_slot: bool,
        fail_write_slot: bool,
        fail_write_meta: bool,
        fail_clear: bool,
    }

    impl FaultyStorage {
        fn healthy(inner: RamStorage, geometry: Geometry) -> Self {
            Self {
                inner,
                geometry,
                fail_read_key: false,
                fail_read_slot: false,
                fail_write_slot: false,
                fail_write_meta: false,
                fail_clear: false,
            }
        }
    }

    impl Storage for FaultyStorage {
        fn geometry(&self) -> Geometry {
            self.geometry
        }

        fn read_meta(&self, slot: u32) -> Result<Meta> {
            self.inner.read_meta(slot)
        }

        fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<()> {
            if self.fail_read_key {
                return Err(Error::Io);
            }
            self.inner.read_key(slot, out)
        }

        fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta> {
            if self.fail_read_slot {
                return Err(Error::Io);
            }
            self.inner.read_slot(slot, key, payload)
        }

        fn write_slot(&mut self, slot: u32, meta: Meta, key: &[u8], payload: &[u8]) -> Result<()> {
            if self.fail_write_slot {
                return Err(Error::Io);
            }
            self.inner.write_slot(slot, meta, key, payload)
        }

        fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<()> {
            if self.fail_write_meta {
                return Err(Error::Io);
            }
            self.inner.write_meta(slot, meta)
        }

        fn clear_all(&mut self) -> Result<()> {
            if self.fail_clear {
                return Err(Error::Io);
            }
            self.inner.clear_all()
        }

        fn sync(&mut self) -> Result<()> {
            Ok(())
        }
    }

    #[test]
    fn table_check_distinguishes_states() {
        let geometry = Geometry {
            capacity: 1,
            key_size: 8,
            payload_size: 4,
        };
        let mut table =
            Table::open(geometry, RamStorage::new(geometry).unwrap()).unwrap();
        let handle = table.allocate(&byte_key(1), &[1; 4]).unwrap();
        assert_eq!(table.check(handle), Ok(HandleStatus::Unchanged));

        table.update(handle, &[2; 4]).unwrap();
        assert_eq!(
            table.check(handle),
            Ok(HandleStatus::Changed(Meta {
                used: true,
                generation: 1,
                version: 2,
            }))
        );

        let current = table.handle(&byte_key(1)).unwrap();
        assert_eq!(table.check(current), Ok(HandleStatus::Unchanged));

        table.free(current).unwrap();
        assert_eq!(table.check(current), Ok(HandleStatus::Removed));

        table.allocate(&byte_key(2), &[3; 4]).unwrap();
        assert_eq!(table.check(current), Ok(HandleStatus::Stale));
        assert_eq!(
            table.check(Handle {
                slot: 1,
                generation: 0,
                version: 0,
            }),
            Err(Error::InvalidArg)
        );
    }

    #[test]
    fn table_check_does_not_read_key_or_payload() {
        let geometry = byte_geometry();
        let mut storage = FaultyStorage::healthy(RamStorage::new(geometry).unwrap(), geometry);
        storage.fail_read_key = true;
        storage.fail_read_slot = true;
        let mut table = Table::open(geometry, storage).unwrap();
        let handle = table
            .allocate(&byte_key(1), &byte_payload(1))
            .unwrap();
        assert_eq!(table.check(handle), Ok(HandleStatus::Unchanged));
    }

    #[test]
    fn table_iter_reports_storage_error() {
        let geometry = byte_geometry();
        let mut storage = FaultyStorage::healthy(RamStorage::new(geometry).unwrap(), geometry);
        storage.fail_read_slot = true;
        let mut table = Table::open(geometry, storage).unwrap();
        table
            .allocate(&byte_key(1), &byte_payload(1))
            .unwrap();
        let mut seen = 0;
        let result = table.iter(|_, _, _, _| seen += 1);
        assert_eq!(result, Err(Error::Io));
        assert_eq!(seen, 0);
    }

    #[test]
    fn table_write_failure_keeps_derived_state() {
        let geometry = byte_geometry();
        let mut inner = RamStorage::new(geometry).unwrap();
        let meta = Meta {
            used: true,
            generation: 1,
            version: 1,
        };
        inner
            .write_slot(0, meta, &byte_key(1), &byte_payload(1))
            .unwrap();

        let mut storage = FaultyStorage::healthy(inner, geometry);
        storage.fail_write_slot = true;
        let mut table = Table::open(geometry, storage).unwrap();
        let handle = table.handle(&byte_key(1)).unwrap();
        let before = table.len();

        assert_eq!(
            table.allocate(&byte_key(2), &byte_payload(2)),
            Err(Error::Io)
        );
        assert_eq!(table.len(), before);
        assert_eq!(table.find(&byte_key(2)), Err(Error::NotFound));
        assert!(table.find(&byte_key(1)).is_ok());

        assert_eq!(table.update(handle, &byte_payload(2)), Err(Error::Io));
        let mut out = [0u8; 12];
        table.get(&byte_key(1), &mut out).unwrap();
        assert_eq!(out, byte_payload(1));
        assert_eq!(table.meta(handle.slot).unwrap().version, 1);
    }

    #[test]
    fn table_meta_write_failure_keeps_derived_state() {
        let geometry = byte_geometry();
        let mut inner = RamStorage::new(geometry).unwrap();
        let meta = Meta {
            used: true,
            generation: 1,
            version: 1,
        };
        inner
            .write_slot(0, meta, &byte_key(1), &byte_payload(1))
            .unwrap();

        let mut storage = FaultyStorage::healthy(inner, geometry);
        storage.fail_write_meta = true;
        let mut table = Table::open(geometry, storage).unwrap();
        let handle = table.handle(&byte_key(1)).unwrap();
        let before = table.len();

        assert_eq!(table.free(handle), Err(Error::Io));
        assert_eq!(table.len(), before);
        assert!(table.find(&byte_key(1)).is_ok());
    }

    #[test]
    fn table_clear_failure_keeps_derived_state() {
        let geometry = byte_geometry();
        let mut inner = RamStorage::new(geometry).unwrap();
        inner
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &byte_key(1),
                &byte_payload(1),
            )
            .unwrap();

        let mut storage = FaultyStorage::healthy(inner, geometry);
        storage.fail_clear = true;
        let mut table = Table::open(geometry, storage).unwrap();
        let before = table.len();

        assert_eq!(table.clear(), Err(Error::Io));
        assert_eq!(table.len(), before);
        assert!(table.find(&byte_key(1)).is_ok());
    }

    #[test]
    fn table_clear_does_not_read_after_clear_all() {
        let geometry = byte_geometry();
        let mut storage = FaultyStorage::healthy(RamStorage::new(geometry).unwrap(), geometry);
        storage.fail_read_slot = true;
        let mut table = Table::open(geometry, storage).unwrap();
        table
            .allocate(&byte_key(1), &byte_payload(1))
            .unwrap();
        table.clear().unwrap();
        assert_eq!(table.len(), 0);
        assert_eq!(table.find(&byte_key(1)), Err(Error::NotFound));
    }

    #[test]
    fn table_collisions_wrap_around_index_end() {
        let mut table = byte_table();
        let last = table.index.len() - 1;
        let mut keys = Vec::new();
        for id in 0..100_000u64 {
            let key = id.to_le_bytes();
            if table.bucket(&key) == last {
                keys.push(key);
                if keys.len() == 3 {
                    break;
                }
            }
        }
        assert_eq!(keys.len(), 3, "three keys in the last bucket");

        let handles: Vec<Handle> = keys
            .iter()
            .enumerate()
            .map(|(index, key)| table.allocate(key, &byte_payload(index as u8)).unwrap())
            .collect();

        table.free(handles[0]).unwrap();
        assert!(table.find(&keys[1]).is_ok());
        assert!(table.find(&keys[2]).is_ok());

        table.free(handles[1]).unwrap();
        assert!(table.find(&keys[2]).is_ok());

        table.free(handles[2]).unwrap();
        table.rebuild().unwrap();
        assert_eq!(table.find(&keys[2]), Err(Error::NotFound));
    }

    struct FaultFlags {
        reads: Cell<usize>,
        fail_read_at: Cell<Option<usize>>,
        fail_write: Cell<bool>,
        fail_erase: Cell<bool>,
    }

    struct FailingFlash {
        inner: SimFlash,
        flags: Rc<FaultFlags>,
    }

    impl FailingFlash {
        fn new(flags: &Rc<FaultFlags>) -> Self {
            Self {
                inner: SimFlash::new(8192, 4096),
                flags: Rc::clone(flags),
            }
        }
    }

    impl NorFlash for FailingFlash {
        fn capacity(&self) -> usize {
            self.inner.capacity()
        }

        fn erase_size(&self) -> usize {
            self.inner.erase_size()
        }

        fn read(&self, offset: usize, out: &mut [u8]) -> core::result::Result<(), FlashError> {
            let reads = self.flags.reads.get();
            self.flags.reads.set(reads + 1);
            if self.flags.fail_read_at.get() == Some(reads) {
                return Err(FlashError::Io);
            }
            self.inner.read(offset, out)
        }

        fn write(
            &mut self,
            offset: usize,
            data: &[u8],
        ) -> core::result::Result<(), FlashError> {
            if self.flags.fail_write.get() {
                return Err(FlashError::Io);
            }
            self.inner.write(offset, data)
        }

        fn erase(&mut self, offset: usize, len: usize) -> core::result::Result<(), FlashError> {
            if self.flags.fail_erase.get() {
                return Err(FlashError::Io);
            }
            self.inner.erase(offset, len)
        }
    }

    fn fault_flags() -> Rc<FaultFlags> {
        Rc::new(FaultFlags {
            reads: Cell::new(0),
            fail_read_at: Cell::new(None),
            fail_write: Cell::new(false),
            fail_erase: Cell::new(false),
        })
    }

    #[test]
    fn ram_flash_reopen_hydrates_and_reuses_generation() {
        let geometry = Geometry {
            capacity: 1,
            key_size: 8,
            payload_size: 0,
        };
        let storage =
            RamFlashStorage::format(SimFlash::new(8192, 4096), geometry, "ram_flash").unwrap();
        let mut table = Table::open(geometry, storage).unwrap();
        let handle = table.allocate(&byte_key(1), &[]).unwrap();
        table.free(handle).unwrap();

        let flash = table.into_storage().into_flash();
        let storage = RamFlashStorage::open(flash, geometry, "ram_flash").unwrap();
        let mut table = Table::open(geometry, storage).unwrap();
        assert_eq!(table.check(handle), Ok(HandleStatus::Removed));

        let reused = table.allocate(&byte_key(1), &[]).unwrap();
        assert_eq!(reused.generation, 2);
    }

    #[test]
    fn ram_flash_write_failure_keeps_ram_and_recovers() {
        let geometry = byte_geometry();
        let flags = fault_flags();
        let flash = FailingFlash::new(&flags);
        let storage = RamFlashStorage::format(flash, geometry, "ram_flash").unwrap();
        let mut table = Table::open(geometry, storage).unwrap();
        let handle = table
            .allocate(&byte_key(1), &byte_payload(1))
            .unwrap();
        let before = table.len();

        flags.fail_write.set(true);
        assert_eq!(
            table.allocate(&byte_key(2), &byte_payload(2)),
            Err(Error::Io)
        );
        assert_eq!(table.len(), before);
        assert!(table.find(&byte_key(1)).is_ok());
        assert_eq!(table.find(&byte_key(2)), Err(Error::NotFound));
        assert_eq!(table.check(handle), Ok(HandleStatus::Unchanged));
        assert_eq!(table.update(handle, &byte_payload(3)), Err(Error::Io));
        let mut out = [0u8; 12];
        table.get(&byte_key(1), &mut out).unwrap();
        assert_eq!(out, byte_payload(1));
        assert_eq!(table.free(handle), Err(Error::Io));
        assert!(table.find(&byte_key(1)).is_ok());
        assert_eq!(table.clear(), Err(Error::Io));
        assert_eq!(table.len(), before);
        assert!(table.find(&byte_key(1)).is_ok());

        flags.fail_write.set(false);
        table.allocate(&byte_key(2), &byte_payload(2)).unwrap();
        assert!(table.update(handle, &byte_payload(3)).unwrap());
        assert!(matches!(
            table.check(handle),
            Ok(HandleStatus::Changed(_))
        ));
    }

    #[test]
    fn ram_flash_compact_keeps_logical_state() {
        let geometry = byte_geometry();
        let storage =
            RamFlashStorage::format(SimFlash::new(8192, 4096), geometry, "ram_flash").unwrap();
        let mut table = Table::open(geometry, storage).unwrap();
        let handle = table
            .allocate(&byte_key(1), &byte_payload(1))
            .unwrap();
        table
            .allocate(&byte_key(2), &byte_payload(2))
            .unwrap();

        let len = table.len();
        table.compact().unwrap();
        assert_eq!(table.len(), len);
        let mut out = [0u8; 12];
        table.get(&byte_key(1), &mut out).unwrap();
        assert_eq!(out, byte_payload(1));
        assert_eq!(table.check(handle), Ok(HandleStatus::Unchanged));

        let flash = table.into_storage().into_flash();
        let storage = RamFlashStorage::open(flash, geometry, "ram_flash").unwrap();
        let table = Table::open(geometry, storage).unwrap();
        table.get(&byte_key(2), &mut out).unwrap();
        assert_eq!(out, byte_payload(2));
    }

    #[test]
    fn ram_flash_hydration_failure_returns_error() {
        let geometry = byte_geometry();
        let flags = fault_flags();
        let flash = FailingFlash::new(&flags);
        let storage = RamFlashStorage::format(flash, geometry, "ram_flash").unwrap();
        let mut table = Table::open(geometry, storage).unwrap();
        table
            .allocate(&byte_key(1), &byte_payload(1))
            .unwrap();
        let flash = table.into_storage().into_flash();

        flags
            .fail_read_at
            .set(Some(flags.reads.get() + 4));
        assert!(RamFlashStorage::open(flash, geometry, "ram_flash").is_err());
    }
}
