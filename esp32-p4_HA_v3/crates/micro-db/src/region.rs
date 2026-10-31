//! Flash region manager: directory, region allocation and bound views.
//!
//! One partition is split into a two-slot ping-pong directory and bump-allocated
//! regions. A region is owned for the lifetime of its `RegionView`; dropping the
//! view only releases runtime ownership, the directory entry stays `USED`.
//!
//! Layout follows V2 `mstore_region.c`:
//!   slot 0..erase_size          directory slot A
//!   erase_size..2*erase_size    directory slot B
//!   2*erase_size..capacity      regions, in allocation order

use std::sync::{Arc, Mutex};

use crate::flash::{crc32, persist_id_bytes, FlashError, NorFlash};
use crate::{Error, Result};

const DIRECTORY_MAGIC: u32 = 0x4D53_4447; // MSDG
const DIRECTORY_REVISION: u16 = 1;
const MAX_ENTRIES: usize = 16;
const HEADER_SIZE: usize = 16;
const ENTRY_SIZE: usize = 32;
const DIRECTORY_SLOT_BYTES: usize = HEADER_SIZE + MAX_ENTRIES * ENTRY_SIZE;
const ENTRY_USED: u16 = 1;
const ENTRY_FREE: u16 = 0;

/// A bound region location, as stored in the directory.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegionLocation {
    pub offset: usize,
    pub size: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Entry {
    id: [u8; 16],
    offset: usize,
    size: usize,
}

#[derive(Clone, Copy)]
struct Owner {
    lease: u64,
    id: [u8; 16],
}

struct RegionState {
    device: Box<dyn NorFlash + Send>,
    erase_size: usize,
    capacity: usize,
    entries: [Option<Entry>; MAX_ENTRIES],
    generation: u32,
    active_slot: usize,
    has_directory: bool,
    owners: Vec<Owner>,
    next_lease: u64,
}

/// Owns the partition directory and hands out region views.
pub struct RegionManager {
    state: Arc<Mutex<RegionState>>,
}

impl RegionManager {
    pub fn new<F: NorFlash + Send + 'static>(device: F) -> Result<Self> {
        let erase_size = device.erase_size();
        let capacity = device.capacity();
        if erase_size < DIRECTORY_SLOT_BYTES {
            return Err(Error::InvalidSize);
        }
        if capacity > u32::MAX as usize || erase_size > usize::MAX / 2 || capacity < 2 * erase_size {
            return Err(Error::InvalidSize);
        }
        let mut state = RegionState {
            device: Box::new(device),
            erase_size,
            capacity,
            entries: [None; MAX_ENTRIES],
            generation: 0,
            active_slot: 1,
            has_directory: false,
            owners: Vec::new(),
            next_lease: 1,
        };
        state.load_directory()?;
        Ok(Self {
            state: Arc::new(Mutex::new(state)),
        })
    }

    pub fn lookup(&self, persist_key: &str) -> Result<RegionLocation> {
        let state = self
            .state
            .lock()
            .map_err(|_| Error::InvalidState)?;
        if !state.has_directory {
            return Err(Error::NotFound);
        }
        let id = persist_id_bytes(persist_key);
        let entry = state
            .entries
            .iter()
            .flatten()
            .find(|entry| entry.id == id)
            .ok_or(Error::NotFound)?;
        Ok(RegionLocation {
            offset: entry.offset,
            size: entry.size,
        })
    }

    pub fn bind(
        &mut self,
        persist_key: &str,
        capacity: usize,
        key_size: usize,
        payload_size: usize,
    ) -> Result<RegionView> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| Error::InvalidState)?;
        let region_size = region_size(capacity, key_size, payload_size, state.erase_size)?;
        let id = persist_id_bytes(persist_key);

        if state.owners.iter().any(|owner| owner.id == id) {
            return Err(Error::InvalidState);
        }

        let existing = state
            .entries
            .iter()
            .flatten()
            .find(|entry| entry.id == id)
            .copied();
        let (offset, new_entries, generation) = match existing {
            Some(entry) => {
                if entry.size != region_size {
                    return Err(Error::InvalidSize);
                }
                if entry.offset < 2 * state.erase_size
                    || entry.offset % state.erase_size != 0
                    || entry.offset > state.capacity
                    || region_size > state.capacity - entry.offset
                {
                    return Err(Error::Corrupt);
                }
                (entry.offset, state.entries, state.generation)
            }
            None => {
                let index = state
                    .entries
                    .iter()
                    .position(Option::is_none)
                    .ok_or(Error::NoSpace)?;
                let offset = 2 * state.erase_size
                    + state
                        .entries
                        .iter()
                        .flatten()
                        .try_fold(0usize, |sum, entry| sum.checked_add(entry.size))
                        .ok_or(Error::InvalidSize)?;
                if offset > state.capacity || region_size > state.capacity - offset {
                    return Err(Error::InvalidSize);
                }
                let mut entries = state.entries;
                entries[index] = Some(Entry {
                    id,
                    offset,
                    size: region_size,
                });
                let generation = state
                    .generation
                    .checked_add(1)
                    .ok_or(Error::Overflow)?;
                (offset, entries, generation)
            }
        };

        let lease = state.next_lease;
        let next_lease = state.next_lease.checked_add(1).ok_or(Error::Overflow)?;
        if new_entries != state.entries {
            state.commit(new_entries, generation)?;
        }

        state.next_lease = next_lease;
        state.owners.push(Owner { lease, id });

        Ok(RegionView {
            state: Arc::clone(&self.state),
            lease,
            base: offset,
            size: region_size,
            erase_size: state.erase_size,
        })
    }
}

impl RegionState {
    fn load_directory(&mut self) -> Result<()> {
        let slot0 = self.read_raw(0)?;
        let slot1 = self.read_raw(self.erase_size)?;
        let erased0 = slot0.iter().all(|byte| *byte == 0xFF);
        let erased1 = slot1.iter().all(|byte| *byte == 0xFF);
        let first = parse_directory(&slot0);
        let second = parse_directory(&slot1);

        let (active_slot, directory) = match (first, second) {
            (Some(a), Some(b)) => {
                if a.0 >= b.0 {
                    (0, a)
                } else {
                    (1, b)
                }
            }
            (Some(a), None) => (0, a),
            (None, Some(b)) => (1, b),
            (None, None) => {
                if erased0 && erased1 {
                    self.has_directory = false;
                    self.active_slot = 1;
                    self.generation = 0;
                    self.entries = [None; MAX_ENTRIES];
                    return Ok(());
                }
                return Err(Error::Corrupt);
            }
        };

        validate_entries(&directory.1, self.erase_size, self.capacity)?;
        self.has_directory = true;
        self.active_slot = active_slot;
        self.generation = directory.0;
        self.entries = directory.1;
        Ok(())
    }

    fn read_raw(&self, base: usize) -> Result<[u8; DIRECTORY_SLOT_BYTES]> {
        let mut raw = [0u8; DIRECTORY_SLOT_BYTES];
        self.device
            .read(base, &mut raw)
            .map_err(Error::from)?;
        Ok(raw)
    }

    fn commit(&mut self, entries: [Option<Entry>; MAX_ENTRIES], generation: u32) -> Result<()> {
        let raw = encode_directory(entries, generation);
        let inactive = 1 - self.active_slot;
        let base = inactive * self.erase_size;
        self.device
            .erase(base, self.erase_size)
            .map_err(Error::from)?;
        self.device
            .write(base + HEADER_SIZE, &raw[HEADER_SIZE..])
            .map_err(Error::from)?;
        self.device
            .write(base, &raw[..HEADER_SIZE])
            .map_err(Error::from)?;
        self.active_slot = inactive;
        self.generation = generation;
        self.has_directory = true;
        self.entries = entries;
        Ok(())
    }
}

impl Drop for RegionView {
    fn drop(&mut self) {
        if let Ok(mut state) = self.state.lock() {
            state.owners.retain(|owner| owner.lease != self.lease);
        }
    }
}

/// Bounds-checked view of one region. It implements `NorFlash`, so the existing
/// `FlashStorage`/`RamFlashStorage` backends can own it directly.
pub struct RegionView {
    state: Arc<Mutex<RegionState>>,
    lease: u64,
    base: usize,
    size: usize,
    erase_size: usize,
}

impl NorFlash for RegionView {
    fn capacity(&self) -> usize {
        self.size
    }

    fn erase_size(&self) -> usize {
        self.erase_size
    }

    fn read(&self, offset: usize, out: &mut [u8]) -> core::result::Result<(), FlashError> {
        let end = offset.checked_add(out.len()).ok_or(FlashError::InvalidSize)?;
        if end > self.size {
            return Err(FlashError::InvalidSize);
        }
        let state = self.state.lock().map_err(|_| FlashError::Corrupt)?;
        state
            .device
            .read(self.base + offset, out)
            .map_err(|error| error)
    }

    fn write(&mut self, offset: usize, data: &[u8]) -> core::result::Result<(), FlashError> {
        let end = offset.checked_add(data.len()).ok_or(FlashError::InvalidSize)?;
        if end > self.size {
            return Err(FlashError::InvalidSize);
        }
        let mut state = self.state.lock().map_err(|_| FlashError::Corrupt)?;
        state
            .device
            .write(self.base + offset, data)
            .map_err(|error| error)
    }

    fn erase(&mut self, offset: usize, len: usize) -> core::result::Result<(), FlashError> {
        if offset % self.erase_size != 0 || len % self.erase_size != 0 {
            return Err(FlashError::InvalidSize);
        }
        let end = offset.checked_add(len).ok_or(FlashError::InvalidSize)?;
        if end > self.size {
            return Err(FlashError::InvalidSize);
        }
        let mut state = self.state.lock().map_err(|_| FlashError::Corrupt)?;
        state
            .device
            .erase(self.base + offset, len)
            .map_err(|error| error)
    }
}

fn region_size(
    capacity: usize,
    key_size: usize,
    payload_size: usize,
    erase_size: usize,
) -> Result<usize> {
    if capacity == 0 || key_size == 0 {
        return Err(Error::InvalidSize);
    }
    if capacity > u32::MAX as usize || key_size > u16::MAX as usize || payload_size > u16::MAX as usize {
        return Err(Error::InvalidSize);
    }
    if erase_size < DIRECTORY_SLOT_BYTES || erase_size > usize::MAX / 2 {
        return Err(Error::InvalidSize);
    }
    let max_record = 16usize
        .checked_add(key_size)
        .and_then(|len| len.checked_add(payload_size))
        .and_then(|len| len.checked_add(8))
        .ok_or(Error::InvalidSize)?;
    let percent = capacity
        .checked_mul(25)
        .ok_or(Error::InvalidSize)?
        / 100;
    let headroom = percent.max(8);
    let records_needed = capacity.checked_add(headroom).ok_or(Error::InvalidSize)?;
    let content = records_needed
        .checked_mul(max_record)
        .and_then(|bytes| bytes.checked_add(64))
        .ok_or(Error::InvalidSize)?;
    let bank_size = content
        .checked_add(erase_size - 1)
        .ok_or(Error::InvalidSize)?
        / erase_size
        * erase_size;
    bank_size.checked_mul(2).ok_or(Error::InvalidSize)
}

fn encode_directory(entries: [Option<Entry>; MAX_ENTRIES], generation: u32) -> [u8; DIRECTORY_SLOT_BYTES] {
    let mut raw = [0xFFu8; DIRECTORY_SLOT_BYTES];
    put_u32(&mut raw, 0, DIRECTORY_MAGIC);
    put_u16(&mut raw, 4, DIRECTORY_REVISION);
    put_u16(&mut raw, 6, MAX_ENTRIES as u16);
    put_u32(&mut raw, 8, generation);
    for (index, entry) in entries.iter().enumerate() {
        let Some(entry) = entry else {
            continue;
        };
        let at = HEADER_SIZE + index * ENTRY_SIZE;
        raw[at..at + 16].copy_from_slice(&entry.id);
        put_u32(&mut raw, at + 16, entry.offset as u32);
        put_u32(&mut raw, at + 20, entry.size as u32);
        put_u16(&mut raw, at + 24, ENTRY_USED);
        put_u16(&mut raw, at + 26, 0);
        let crc = crc32(&raw[at..at + 28]);
        put_u32(&mut raw, at + 28, crc);
    }
    let crc = crc32(&raw[..12]);
    put_u32(&mut raw, 12, crc);
    raw
}

fn parse_directory(raw: &[u8; DIRECTORY_SLOT_BYTES]) -> Option<(u32, [Option<Entry>; MAX_ENTRIES])> {
    if get_u32(raw, 0) != DIRECTORY_MAGIC
        || get_u16(raw, 4) != DIRECTORY_REVISION
        || get_u16(raw, 6) != MAX_ENTRIES as u16
        || crc32(&raw[..12]) != get_u32(raw, 12)
    {
        return None;
    }
    let mut entries = [None; MAX_ENTRIES];
    for (index, slot) in entries.iter_mut().enumerate() {
        let at = HEADER_SIZE + index * ENTRY_SIZE;
        if raw[at..at + ENTRY_SIZE].iter().all(|byte| *byte == 0xFF) {
            continue;
        }
        if crc32(&raw[at..at + 28]) != get_u32(raw, at + 28) {
            return None;
        }
        let state = get_u16(raw, at + 24);
        if state != ENTRY_USED && state != ENTRY_FREE {
            return None;
        }
        if state == ENTRY_FREE {
            continue;
        }
        let mut id = [0u8; 16];
        id.copy_from_slice(&raw[at..at + 16]);
        *slot = Some(Entry {
            id,
            offset: get_u32(raw, at + 16) as usize,
            size: get_u32(raw, at + 20) as usize,
        });
    }
    Some((get_u32(raw, 8), entries))
}

fn validate_entries(entries: &[Option<Entry>; MAX_ENTRIES], erase_size: usize, capacity: usize) -> Result<()> {
    let region_align = erase_size.checked_mul(2).ok_or(Error::InvalidSize)?;
    for entry in entries.iter().flatten() {
        if entry.size == 0
            || entry.size % region_align != 0
            || entry.offset < 2 * erase_size
            || entry.offset % erase_size != 0
            || entry.offset > capacity
            || entry.size > capacity - entry.offset
        {
            return Err(Error::Corrupt);
        }
    }
    Ok(())
}

fn put_u16(buf: &mut [u8], offset: usize, value: u16) {
    buf[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(buf: &mut [u8], offset: usize, value: u32) {
    buf[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn get_u16(buf: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([buf[offset], buf[offset + 1]])
}

fn get_u32(buf: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        buf[offset],
        buf[offset + 1],
        buf[offset + 2],
        buf[offset + 3],
    ])
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::SimFlash;
    use crate::{Geometry, RamFlashStorage, Table};
    use std::sync::{Arc, Mutex};

    const ERASE: usize = 4096;
    const REGION: usize = 8192;

    #[derive(Clone)]
    struct SharedFlash(Arc<Mutex<SimFlash>>);

    impl SharedFlash {
        fn new() -> Self {
            Self(Arc::new(Mutex::new(SimFlash::new(32768, ERASE))))
        }

        fn corrupt(&self, offset: usize, data: &[u8]) {
            self.0.lock().unwrap().corrupt(offset, data);
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

    fn new_manager(flash: &SharedFlash) -> RegionManager {
        RegionManager::new(flash.clone()).unwrap()
    }

    fn bind(manager: &mut RegionManager, key: &str) -> RegionView {
        manager.bind(key, 4, 8, 0).unwrap()
    }

    #[test]
    fn fresh_has_no_directory() {
        let flash = SharedFlash::new();
        let manager = new_manager(&flash);
        assert_eq!(manager.lookup("a"), Err(Error::NotFound));
    }

    #[test]
    fn bind_lookup_lease_and_rebind() {
        let flash = SharedFlash::new();
        let mut manager = new_manager(&flash);
        let first = bind(&mut manager, "a");
        assert_eq!(first.capacity(), REGION);
        assert_eq!(first.erase_size(), ERASE);
        assert_eq!(
            manager.lookup("a"),
            Ok(RegionLocation {
                offset: 2 * ERASE,
                size: REGION
            })
        );

        assert!(manager.bind("a", 4, 8, 0).is_err());
        let second = bind(&mut manager, "b");
        assert_eq!(
            manager.lookup("b"),
            Ok(RegionLocation {
                offset: 2 * ERASE + REGION,
                size: REGION
            })
        );

        drop(first);
        let rebound = bind(&mut manager, "a");
        assert_eq!(
            manager.lookup("a"),
            Ok(RegionLocation {
                offset: 2 * ERASE,
                size: REGION
            })
        );

        drop(second);
        drop(rebound);
        let reopened = new_manager(&flash);
        assert!(reopened.lookup("a").is_ok());
        assert!(reopened.lookup("b").is_ok());
    }

    #[test]
    fn region_view_bounds_and_erase_alignment() {
        let flash = SharedFlash::new();
        let mut manager = new_manager(&flash);
        let mut view = bind(&mut manager, "a");
        view.write(0, &[0xAA]).unwrap();
        let mut byte = [0u8; 1];
        view.read(0, &mut byte).unwrap();
        assert_eq!(byte, [0xAA]);
        assert_eq!(view.read(REGION, &mut byte), Err(FlashError::InvalidSize));
        assert_eq!(view.erase(1, ERASE), Err(FlashError::InvalidSize));
        view.erase(0, ERASE).unwrap();
    }

    #[test]
    fn reopen_keeps_offsets_and_regions() {
        let flash = SharedFlash::new();
        {
            let mut manager = new_manager(&flash);
            let a = bind(&mut manager, "a");
            let b = bind(&mut manager, "b");
            drop(a);
            drop(b);
        }
        let mut manager = new_manager(&flash);
        assert_eq!(
            manager.lookup("a"),
            Ok(RegionLocation {
                offset: 2 * ERASE,
                size: REGION
            })
        );
        assert_eq!(
            manager.lookup("b"),
            Ok(RegionLocation {
                offset: 2 * ERASE + REGION,
                size: REGION
            })
        );
        let a = bind(&mut manager, "a");
        drop(a);
    }

    #[test]
    fn region_view_works_with_flash_storage() {
        let flash = SharedFlash::new();
        let mut manager = new_manager(&flash);
        let geometry = Geometry {
            capacity: 4,
            key_size: 8,
            payload_size: 0,
        };
        let view = manager.bind("a", 4, 8, 0).unwrap();
        let storage = RamFlashStorage::format(view, geometry, "a").unwrap();
        let mut table = Table::open(geometry, storage).unwrap();
        table.allocate(&[1u8; 8], &[]).unwrap();
        drop(table);

        let view = manager.bind("a", 4, 8, 0).unwrap();
        let storage = RamFlashStorage::open(view, geometry, "a").unwrap();
        let table = Table::open(geometry, storage).unwrap();
        assert!(table.find(&[1u8; 8]).is_ok());
    }

    #[test]
    fn forgotten_lease_keeps_region_busy() {
        let flash = SharedFlash::new();
        let mut manager = new_manager(&flash);
        let view = bind(&mut manager, "a");
        std::mem::forget(view);
        assert!(manager.bind("a", 4, 8, 0).is_err());
    }

    #[test]
    fn corrupt_directory_is_not_fresh() {
        let flash = SharedFlash::new();
        {
            let mut manager = new_manager(&flash);
            let view = bind(&mut manager, "a");
            drop(view);
        }
        flash.corrupt(0, &[0x00]);
        assert_eq!(RegionManager::new(flash.clone()).err(), Some(Error::Corrupt));
    }

    #[test]
    fn partially_written_directory_is_corrupt() {
        let flash = SharedFlash::new();
        flash.corrupt(0, &[0x00]);
        assert_eq!(RegionManager::new(flash.clone()).err(), Some(Error::Corrupt));
    }

    #[test]
    fn region_geometry_mismatch_is_rejected() {
        let flash = SharedFlash::new();
        let mut manager = new_manager(&flash);
        let view = bind(&mut manager, "a");
        drop(view);
        assert_eq!(
            manager.bind("a", 4, 8, 4000).err(),
            Some(Error::InvalidSize)
        );
    }

    #[test]
    fn directory_full_is_no_space() {
        let flash = SharedFlash(Arc::new(Mutex::new(SimFlash::new(17 * REGION, ERASE))));
        let mut manager = new_manager(&flash);
        let mut views = Vec::new();
        for index in 0..16 {
            views.push(manager.bind(&format!("key{index}"), 4, 8, 0).unwrap());
        }
        assert_eq!(
            manager.bind("overflow", 4, 8, 0).err(),
            Some(Error::NoSpace)
        );
    }
}