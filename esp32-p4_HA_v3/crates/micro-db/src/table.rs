use core::borrow::Borrow;
use core::hash::{Hash, Hasher};

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

/// Safe cached reference to one slot life: valid while the slot is used and its
/// generation is unchanged.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Handle {
    pub slot: u32,
    pub generation: u32,
}

/// Borrowed snapshot: meta + key + payload, read under one borrow.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct View<'a, K, V> {
    pub slot: u32,
    pub meta: Meta,
    pub key: &'a K,
    pub payload: &'a V,
}

#[derive(Clone)]
struct Slot<K, V> {
    meta: Meta,
    data: Option<(K, V)>,
}

impl<K, V> Slot<K, V> {
    fn free() -> Self {
        Self {
            meta: Meta::default(),
            data: None,
        }
    }
}

#[derive(Clone, Copy)]
struct IndexEntry {
    slot: u32,
    home: u32,
}

const NONE: u32 = u32::MAX;

/// Fixed-capacity slot table. `key` is owned by the table, `payload` by the
/// caller; writes require `&mut self`, so views (borrowed from `&self`) cannot
/// outlive a mutation. Long-lived references are [`Handle`] values.
pub struct Table<K, V> {
    slots: Vec<Slot<K, V>>,
    free: Vec<u32>,
    index: Vec<Option<IndexEntry>>,
    index_of_slot: Vec<u32>,
    live_count: usize,
}

impl<K, V> Table<K, V> {
    pub fn new(capacity: usize) -> Result<Self> {
        if capacity == 0 || capacity >= u32::MAX as usize {
            return Err(Error::InvalidSize);
        }
        let index_capacity = capacity.checked_mul(2).ok_or(Error::InvalidSize)?.next_power_of_two();
        Ok(Self {
            slots: (0..capacity).map(|_| Slot::free()).collect(),
            free: (0..capacity as u32).collect(),
            index: vec![None; index_capacity],
            index_of_slot: vec![NONE; capacity],
            live_count: 0,
        })
    }

    pub fn capacity(&self) -> usize {
        self.slots.len()
    }

    pub fn len(&self) -> usize {
        self.live_count
    }

    pub fn is_empty(&self) -> bool {
        self.live_count == 0
    }

    pub fn meta(&self, slot: u32) -> Result<Meta> {
        Ok(self.slot(slot)?.meta)
    }

    /// `key -> slot`. Uses the derived index, never a linear scan.
    pub fn find<Q>(&self, key: &Q) -> Result<u32>
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        let mut bucket = self.home(key);
        loop {
            match self.index[bucket as usize] {
                None => return Err(Error::NotFound),
                Some(entry) => {
                    let data = &self.slots[entry.slot as usize].data;
                    if matches!(data, Some((stored, _)) if stored.borrow() == key) {
                        return Ok(entry.slot);
                    }
                    bucket = self.next_bucket(bucket);
                }
            }
        }
    }

    pub fn handle(&self, key: &K) -> Result<Handle>
    where
        K: Eq + Hash,
    {
        let slot = self.find(key)?;
        let meta = self.slot(slot)?.meta;
        Ok(Handle {
            slot,
            generation: meta.generation,
        })
    }

    /// Borrowed snapshot; payload is not copied.
    pub fn read(&self, slot: u32) -> Result<View<'_, K, V>> {
        let entry = self.slot(slot)?;
        match &entry.data {
            Some((key, payload)) if entry.meta.used => Ok(View {
                slot,
                meta: entry.meta,
                key,
                payload,
            }),
            _ => Err(Error::Stale),
        }
    }

    pub fn read_handle(&self, handle: Handle) -> Result<View<'_, K, V>> {
        let view = self.read(handle.slot)?;
        if view.meta.generation != handle.generation {
            return Err(Error::Stale);
        }
        Ok(view)
    }

    pub fn allocate(&mut self, key: K, payload: V) -> Result<Handle>
    where
        K: Eq + Hash,
    {
        if self.find(&key).is_ok() {
            return Err(Error::AlreadyExists);
        }
        if self.live_count == self.slots.len() {
            return Err(Error::NoSpace);
        }
        // Slots whose generation is exhausted can never be reused (no ABA).
        let position = self
            .free
            .iter()
            .rposition(|&slot| self.slots[slot as usize].meta.generation != u32::MAX)
            .ok_or(Error::Overflow)?;
        let slot = self.free.swap_remove(position);
        let entry = &mut self.slots[slot as usize];
        entry.meta.used = true;
        entry.meta.generation += 1;
        entry.meta.version = 1;
        entry.data = Some((key, payload));
        let generation = entry.meta.generation;
        self.index_insert(slot);
        self.live_count += 1;
        Ok(Handle { slot, generation })
    }

    /// Returns whether payload actually changed; `version` grows only then.
    pub fn update(&mut self, handle: Handle, payload: V) -> Result<bool>
    where
        V: PartialEq,
    {
        let entry = self.checked_mut(handle)?;
        if entry.data.as_ref().is_some_and(|(_, old)| *old == payload) {
            return Ok(false);
        }
        if entry.meta.version == u32::MAX {
            return Err(Error::Overflow);
        }
        entry.data.as_mut().expect("checked").1 = payload;
        entry.meta.version += 1;
        Ok(true)
    }

    pub fn free(&mut self, handle: Handle) -> Result<()> {
        let slot = handle.slot;
        self.checked(handle)?;
        self.index_remove(slot);
        let entry = &mut self.slots[slot as usize];
        entry.meta.used = false;
        entry.data = None;
        self.free.push(slot);
        self.live_count -= 1;
        Ok(())
    }

    pub fn iter(&self) -> impl Iterator<Item = View<'_, K, V>> {
        self.slots.iter().enumerate().filter_map(move |(slot, entry)| {
            match (&entry.data, entry.meta.used) {
                (Some((key, payload)), true) => Some(View {
                    slot: slot as u32,
                    meta: entry.meta,
                    key,
                    payload,
                }),
                _ => None,
            }
        })
    }

    /// Discards every slot (canonical bytes and derived state).
    pub fn clear(&mut self) {
        for slot in &mut self.slots {
            *slot = Slot::free();
        }
        self.index.fill(None);
        self.index_of_slot.fill(NONE);
        self.free.clear();
        self.free.extend(0..self.slots.len() as u32);
        self.live_count = 0;
    }

    /// Rebuilds index / free list / live count from canonical slots.
    pub fn rebuild(&mut self)
    where
        K: Eq + Hash,
    {
        self.index.fill(None);
        self.index_of_slot.fill(NONE);
        self.free.clear();
        self.live_count = 0;
        for slot in 0..self.slots.len() as u32 {
            if self.slots[slot as usize].meta.used {
                self.index_insert(slot);
                self.live_count += 1;
            } else {
                self.free.push(slot);
            }
        }
    }

    pub(crate) fn restore(&mut self, key: K, payload: V, meta: Meta) -> Result<()>
    where
        K: Eq + Hash,
    {
        if self.find(&key).is_ok() {
            return Err(Error::AlreadyExists);
        }
        let position = self
            .free
            .iter()
            .rposition(|&slot| self.slots[slot as usize].meta.generation != u32::MAX)
            .ok_or(Error::Overflow)?;
        let slot = self.free.swap_remove(position);
        let entry = &mut self.slots[slot as usize];
        entry.meta = Meta {
            used: true,
            ..meta
        };
        entry.data = Some((key, payload));
        self.index_insert(slot);
        self.live_count += 1;
        Ok(())
    }

    fn checked(&self, handle: Handle) -> Result<()> {
        let entry = self.slot(handle.slot)?;
        if !entry.meta.used || entry.meta.generation != handle.generation {
            return Err(Error::Stale);
        }
        Ok(())
    }

    fn checked_mut(&mut self, handle: Handle) -> Result<&mut Slot<K, V>> {
        self.checked(handle)?;
        Ok(&mut self.slots[handle.slot as usize])
    }

    fn slot(&self, slot: u32) -> Result<&Slot<K, V>> {
        self.slots.get(slot as usize).ok_or(Error::InvalidArg)
    }

    fn next_bucket(&self, bucket: u32) -> u32 {
        (bucket + 1) & (self.index.len() as u32 - 1)
    }

    fn home<Q>(&self, key: &Q) -> u32
    where
        Q: Hash + ?Sized,
    {
        let mut hasher = Fnv1a::default();
        key.hash(&mut hasher);
        (hasher.finish() as u32) & (self.index.len() as u32 - 1)
    }

    fn index_insert(&mut self, slot: u32)
    where
        K: Hash,
    {
        let key = &self.slots[slot as usize].data.as_ref().expect("live slot").0;
        let mut bucket = self.home(key);
        while self.index[bucket as usize].is_some() {
            bucket = self.next_bucket(bucket);
        }
        self.index[bucket as usize] = Some(IndexEntry {
            slot,
            home: bucket,
        });
        self.index_of_slot[slot as usize] = bucket;
    }

    /// Backward-shift deletion keeps probing chains intact without tombstones.
    fn index_remove(&mut self, slot: u32) {
        let mut hole = self.index_of_slot[slot as usize];
        if hole == NONE {
            return;
        }
        self.index_of_slot[slot as usize] = NONE;
        self.index[hole as usize] = None;

        let mut scan = self.next_bucket(hole);
        while let Some(entry) = self.index[scan as usize] {
            let home = entry.home;
            let stays = if hole <= scan {
                hole < home && home <= scan
            } else {
                hole < home || home <= scan
            };
            if stays {
                scan = self.next_bucket(scan);
                continue;
            }
            self.index[hole as usize] = Some(entry);
            self.index_of_slot[entry.slot as usize] = hole;
            self.index[scan as usize] = None;
            hole = scan;
            scan = self.next_bucket(scan);
        }
    }
}

#[derive(Default)]
struct Fnv1a(u64);

impl Hasher for Fnv1a {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        let mut hash = if self.0 == 0 { 0xcbf29ce484222325 } else { self.0 };
        for byte in bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
        self.0 = hash;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ring::Ring;

    fn table() -> Table<u32, (i32, i32)> {
        Table::new(4).unwrap()
    }

    #[test]
    fn allocate_find_read_and_meta() {
        let mut t = table();
        let key = 10;
        let handle = t.allocate(key, (1, 1)).unwrap();
        assert_eq!(handle.generation, 1);
        assert_eq!(t.find(&key).unwrap(), handle.slot);
        assert_eq!(t.handle(&key).unwrap(), handle);
        assert_eq!(t.len(), 1);

        let view = t.read(handle.slot).unwrap();
        assert_eq!(view.key, &key);
        assert_eq!(view.payload, &(1, 1));
        assert_eq!(view.meta.version, 1);
        assert_eq!(t.meta(handle.slot).unwrap(), view.meta);
    }

    #[test]
    fn duplicate_key_and_capacity_are_rejected() {
        let mut t = Table::new(2).unwrap();
        t.allocate(1u32, (0, 0)).unwrap();
        assert_eq!(t.allocate(1, (0, 0)), Err(Error::AlreadyExists));
        t.allocate(2, (0, 0)).unwrap();
        assert_eq!(t.allocate(3, (0, 0)), Err(Error::NoSpace));
    }

    #[test]
    fn update_versions_only_when_payload_changes() {
        let mut t = table();
        let h = t.allocate(7, (1, 1)).unwrap();
        assert_eq!(t.update(h, (1, 1)), Ok(false));
        assert_eq!(t.read(h.slot).unwrap().meta.version, 1);
        assert_eq!(t.update(h, (2, 2)), Ok(true));
        assert_eq!(t.read(h.slot).unwrap().meta.version, 2);
        assert_eq!(t.read(h.slot).unwrap().payload, &(2, 2));
    }

    #[test]
    fn stale_and_free_handles_are_rejected() {
        let mut t = table();
        let h = t.allocate(7, (1, 1)).unwrap();
        assert_eq!(t.update(Handle { slot: h.slot, generation: h.generation + 1 }, (0, 0)), Err(Error::Stale));
        assert_eq!(t.update(Handle { slot: 99, generation: 1 }, (0, 0)), Err(Error::InvalidArg));
        t.free(h).unwrap();
        assert_eq!(t.read(h.slot), Err(Error::Stale));
        assert_eq!(t.update(h, (0, 0)), Err(Error::Stale));
        assert_eq!(t.free(h), Err(Error::Stale));
        assert_eq!(t.find(&7), Err(Error::NotFound));
    }

    #[test]
    fn slot_reuse_bumps_generation_and_resets_version() {
        let mut t = Table::new(1).unwrap();
        let first = t.allocate(1, (1, 1)).unwrap();
        t.free(first).unwrap();
        let second = t.allocate(2, (2, 2)).unwrap();
        assert_eq!(second.slot, first.slot);
        assert_eq!(second.generation, first.generation + 1);
        assert_eq!(t.read(second.slot).unwrap().meta.version, 1);
        assert_eq!(t.read(second.slot).unwrap().key, &2);
    }

    #[test]
    fn index_stays_correct_after_middle_free() {
        let mut t = table();
        let a = t.allocate(1, (1, 0)).unwrap();
        let b = t.allocate(2, (2, 0)).unwrap();
        let c = t.allocate(3, (3, 0)).unwrap();
        t.free(b).unwrap();
        assert_eq!(t.find(&1).unwrap(), a.slot);
        assert_eq!(t.find(&3).unwrap(), c.slot);
        t.rebuild();
        assert_eq!(t.find(&1).unwrap(), a.slot);
        assert_eq!(t.find(&3).unwrap(), c.slot);
        assert_eq!(t.find(&2), Err(Error::NotFound));
        assert_eq!(t.len(), 2);
    }

    #[test]
    fn generation_exhaustion_returns_overflow() {
        let mut t = Table::new(1).unwrap();
        let h = t.allocate(1, (0, 0)).unwrap();
        t.free(h).unwrap();
        t.slots[h.slot as usize].meta.generation = u32::MAX;
        assert_eq!(t.allocate(2, (0, 0)), Err(Error::Overflow));
    }

    #[test]
    fn version_exhaustion_returns_overflow_without_change() {
        let mut t = table();
        let h = t.allocate(1, (1, 1)).unwrap();
        t.slots[h.slot as usize].meta.version = u32::MAX;
        assert_eq!(t.update(h, (2, 2)), Err(Error::Overflow));
        assert_eq!(t.read(h.slot).unwrap().meta.version, u32::MAX);
        assert_eq!(t.read(h.slot).unwrap().payload, &(1, 1));
        assert_eq!(t.update(h, (1, 1)), Ok(false));
    }

    #[test]
    fn clear_and_rebuild_restore_invariants() {
        let mut t = table();
        t.allocate(1, (1, 0)).unwrap();
        t.allocate(2, (2, 0)).unwrap();
        t.rebuild();
        assert_eq!(t.len(), 2);
        t.clear();
        assert_eq!(t.len(), 0);
        assert_eq!(t.find(&1), Err(Error::NotFound));
        let h = t.allocate(3, (3, 0)).unwrap();
        assert_eq!(h.generation, 1);
    }

    #[test]
    fn iter_yields_only_live_slots() {
        let mut t = table();
        let a = t.allocate(1, (1, 0)).unwrap();
        t.allocate(2, (2, 0)).unwrap();
        t.free(a).unwrap();
        let keys: Vec<u32> = t.iter().map(|view| *view.key).collect();
        assert_eq!(keys, vec![2]);
    }

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
}
