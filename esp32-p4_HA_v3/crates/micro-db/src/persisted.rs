//! RAM table + flash journal with consistent mutations.
//!
//! Order for writes: validate and reserve flash space first, apply the RAM
//! mutation, append the record, and undo RAM if the append fails. Removals append
//! a tombstone before freeing the RAM slot, so a reboot cannot resurrect a device.
//! Flash layout/CRC stays private to `flash`; Domain sees only `PersistedTable`.

use core::hash::Hash;

use crate::flash::{FlashError, FlashMeta, FlashTable, Geometry, NorFlash, KIND_SET};
use crate::{Error, Handle, Meta, Table};

pub trait Fixed: Copy + Eq + Hash {
    const SIZE: usize;
    fn to_bytes(&self, out: &mut [u8]);
    fn from_bytes(bytes: &[u8]) -> Self;
}

impl<const N: usize> Fixed for [u8; N] {
    const SIZE: usize = N;

    fn to_bytes(&self, out: &mut [u8]) {
        out[..N].copy_from_slice(self);
    }

    fn from_bytes(bytes: &[u8]) -> Self {
        let mut out = [0u8; N];
        out.copy_from_slice(&bytes[..N]);
        out
    }
}

impl From<FlashError> for Error {
    fn from(error: FlashError) -> Self {
        match error {
            FlashError::Io => Error::Io,
            FlashError::Corrupt => Error::Corrupt,
            FlashError::NoSpace => Error::NoSpace,
            FlashError::InvalidSize => Error::InvalidSize,
            FlashError::NotFound => Error::NotFound,
            FlashError::InvalidState => Error::InvalidState,
        }
    }
}

pub struct PersistedTable<K: Fixed, V: Fixed> {
    table: Table<K, V>,
    flash: FlashTable,
    /// Flash journal slot -> key; `None` means the slot is free/reusable.
    journal: Vec<Option<K>>,
    /// Reused encode buffers: a write must not allocate on the hot path.
    key_buf: Vec<u8>,
    payload_buf: Vec<u8>,
}

impl<K: Fixed, V: Fixed> PersistedTable<K, V> {
    pub fn open<F: NorFlash>(
        flash: &mut F,
        capacity: usize,
        persist_id: &str,
    ) -> Result<Self, Error> {
        let geometry = Geometry {
            capacity,
            key_size: K::SIZE,
            payload_size: V::SIZE,
        };
        let flash_table = FlashTable::open(flash, geometry, persist_id)?;
        let mut table = Table::new(capacity)?;
        let mut journal = vec![None; capacity];
        let mut key_bytes = vec![0u8; K::SIZE];
        let mut payload_bytes = vec![0u8; V::SIZE];

        for slot in 0..capacity as u32 {
            match flash_table.read_slot(flash, slot, &mut key_bytes, &mut payload_bytes) {
                Ok((meta, KIND_SET)) if meta.used => {
                    let key = K::from_bytes(&key_bytes);
                    let payload = V::from_bytes(&payload_bytes);
                    table.restore(
                        key,
                        payload,
                        Meta {
                            used: true,
                            generation: meta.generation,
                            version: meta.version,
                        },
                    )?;
                    journal[slot as usize] = Some(key);
                }
                Ok(_) => {}
                Err(FlashError::NotFound) => {}
                Err(error) => return Err(error.into()),
            }
        }
        let _ = persist_id;
        Ok(Self {
            table,
            flash: flash_table,
            journal,
            key_buf: vec![0u8; K::SIZE],
            payload_buf: vec![0u8; V::SIZE],
        })
    }

    pub fn format<F: NorFlash>(
        flash: &mut F,
        capacity: usize,
        persist_id: &str,
    ) -> Result<Self, Error> {
        let geometry = Geometry {
            capacity,
            key_size: K::SIZE,
            payload_size: V::SIZE,
        };
        let flash_table = FlashTable::format(flash, geometry, persist_id)?;
        Ok(Self {
            table: Table::new(capacity)?,
            flash: flash_table,
            journal: vec![None; capacity],
            key_buf: vec![0u8; K::SIZE],
            payload_buf: vec![0u8; V::SIZE],
        })
    }

    pub fn get(&self, key: &K, out: &mut V) -> Result<(), Error> {
        let handle = self.table.handle(key)?;
        let view = self.table.read(handle.slot)?;
        *out = *view.payload;
        Ok(())
    }

    pub fn meta(&self, key: &K) -> Result<Meta, Error> {
        let handle = self.table.handle(key)?;
        self.table.meta(handle.slot)
    }

    pub fn put<F: NorFlash>(
        &mut self,
        flash: &mut F,
        key: K,
        payload: V,
    ) -> Result<(Handle, bool), Error> {
        let journal_slot = match self
            .journal
            .iter()
            .position(|slot| slot.as_ref() == Some(&key))
        {
            Some(slot) => slot,
            None => self
                .journal
                .iter()
                .position(Option::is_none)
                .ok_or(Error::NoSpace)?,
        };
        let existing = self.table.handle(&key).ok();
        let old_payload = match existing {
            Some(handle) => Some(*self.table.read(handle.slot)?.payload),
            None => None,
        };
        let (handle, changed) = match existing {
            Some(handle) => (handle, self.table.update(handle, payload)?),
            None => (self.table.allocate(key, payload)?, true),
        };
        if !changed {
            return Ok((handle, false));
        }

        let meta = self.table.meta(handle.slot)?;
        key.to_bytes(&mut self.key_buf);
        payload.to_bytes(&mut self.payload_buf);
        let flash_meta = FlashMeta {
            used: true,
            generation: meta.generation,
            version: meta.version,
        };
        match self.flash.append_set(
            flash,
            journal_slot as u32,
            flash_meta,
            &self.key_buf,
            &self.payload_buf,
        ) {
            Ok(()) => {
                self.journal[journal_slot] = Some(key);
                Ok((handle, true))
            }
            Err(error) => {
                if let Some(old) = old_payload {
                    let _ = self.table.update(handle, old);
                } else {
                    let _ = self.table.free(handle);
                }
                Err(error.into())
            }
        }
    }

    pub fn remove<F: NorFlash>(&mut self, flash: &mut F, key: &K) -> Result<(), Error> {
        let handle = self.table.handle(key)?;
        let journal_slot = self
            .journal
            .iter()
            .position(|slot| slot.as_ref() == Some(key))
            .ok_or(Error::NotFound)?;
        let meta = self.table.meta(handle.slot)?;
        self.flash.append_meta(
            flash,
            journal_slot as u32,
            FlashMeta {
                used: false,
                generation: meta.generation,
                version: meta.version,
            },
        )?;
        self.table.free(handle)?;
        self.journal[journal_slot] = None;
        Ok(())
    }

    /// Moves the journal to the other bank. RAM handles/slots are unchanged:
    /// slot, generation and version are copied by the flash backend.
    pub fn compact<F: NorFlash>(&mut self, flash: &mut F) -> Result<(), Error> {
        self.flash.compact(flash).map_err(Into::into)
    }

    pub fn bank_state(&self) -> (usize, u32) {
        self.flash.bank_state()
    }

    pub fn len(&self) -> usize {
        self.table.len()
    }

    pub fn is_empty(&self) -> bool {
        self.table.is_empty()
    }

    pub fn iter(&self) -> impl Iterator<Item = crate::View<'_, K, V>> {
        self.table.iter()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::flash::sim::SimFlash;

    fn region() -> SimFlash {
        SimFlash::new(4096, 2048)
    }

    fn values() -> [u8; 12] {
        [3; 12]
    }

    #[test]
    fn put_update_and_reboot_restore() {
        let mut flash = region();
        let mut store: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::format(&mut flash, 4, "devices").unwrap();
        let key = [1u8; 8];
        let (_, changed) = store.put(&mut flash, key, values()).unwrap();
        assert!(changed);
        let mut out = [0u8; 12];
        store.get(&key, &mut out).unwrap();
        assert_eq!(out, values());

        let next = [9u8; 12];
        store.put(&mut flash, key, next).unwrap();
        let restarted = PersistedTable::open(&mut flash, 4, "devices").unwrap();
        restarted.get(&key, &mut out).unwrap();
        assert_eq!(out, next);
        assert_eq!(restarted.len(), 1);
    }

    #[test]
    fn removal_is_tombstoned_across_reboot() {
        let mut flash = region();
        let mut store: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::format(&mut flash, 4, "devices").unwrap();
        let key = [2u8; 8];
        store.put(&mut flash, key, values()).unwrap();
        store.remove(&mut flash, &key).unwrap();
        assert_eq!(store.len(), 0);

        let restarted = PersistedTable::open(&mut flash, 4, "devices").unwrap();
        let mut out = [0u8; 12];
        assert_eq!(restarted.get(&key, &mut out), Err(Error::NotFound));
        assert_eq!(restarted.len(), 0);
    }

    #[test]
    fn unchanged_update_does_not_append() {
        let mut flash = region();
        let mut store: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::format(&mut flash, 4, "devices").unwrap();
        let key = [3u8; 8];
        store.put(&mut flash, key, values()).unwrap();
        let writes = flash.writes;
        let (_, changed) = store.put(&mut flash, key, values()).unwrap();
        assert!(!changed);
        assert_eq!(flash.writes, writes);
    }

    #[test]
    fn flash_failure_undoes_ram_mutation() {
        let mut flash = region();
        let mut store: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::format(&mut flash, 4, "devices").unwrap();
        let key = [4u8; 8];
        flash.fail_next_write = true;
        assert_eq!(store.put(&mut flash, key, values()), Err(Error::Io));
        let mut out = [0u8; 12];
        assert_eq!(store.get(&key, &mut out), Err(Error::NotFound));
        assert_eq!(store.len(), 0);
    }

    #[test]
    fn no_space_leaves_ram_unchanged() {
        let mut flash = SimFlash::new(512, 256);
        let mut store: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::format(&mut flash, 4, "devices").unwrap();
        let key = [5u8; 8];
        store.put(&mut flash, key, values()).unwrap();
        store.remove(&mut flash, &key).unwrap();
        // Fill the journal until the flash headroom is gone.
        for index in 0..4u8 {
            if store.put(&mut flash, [index; 8], values()).is_err() {
                break;
            }
        }
        let before = store.len();
        let result = store.put(&mut flash, [9u8; 8], values());
        assert_eq!(result, Err(Error::NoSpace));
        assert_eq!(store.len(), before);
        let mut out = [0u8; 12];
        assert_eq!(store.get(&[9u8; 8], &mut out), Err(Error::NotFound));
    }

    #[test]
    fn failed_allocate_restores_free_list() {
        let mut flash = SimFlash::new(1024, 512);
        let mut store: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::format(&mut flash, 2, "devices").unwrap();
        flash.fail_next_write = true;
        assert_eq!(store.put(&mut flash, [1; 8], values()), Err(Error::Io));
        assert_eq!(store.len(), 0);
        assert!(store.put(&mut flash, [1; 8], values()).is_ok());
        assert!(store.put(&mut flash, [2; 8], values()).is_ok());
        assert_eq!(store.len(), 2);
        assert_eq!(store.put(&mut flash, [3; 8], values()), Err(Error::NoSpace));
    }

    #[test]
    fn capacity_growth_is_invalid_state() {
        let mut flash = SimFlash::new(8192, 4096);
        PersistedTable::<[u8; 8], [u8; 12]>::format(&mut flash, 4, "devices").unwrap();
        assert_eq!(
            PersistedTable::<[u8; 8], [u8; 12]>::open(&mut flash, 8, "devices").err(),
            Some(Error::InvalidState)
        );
        assert_eq!(
            PersistedTable::<[u8; 8], [u8; 12]>::open(&mut flash, 2, "devices").err(),
            Some(Error::InvalidState)
        );
    }

    #[test]
    fn open_initializes_erased_partition() {
        let mut flash = SimFlash::new(8192, 4096);
        let mut store: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::open(&mut flash, 4, "devices").unwrap();
        assert_eq!(store.len(), 0);
        store.put(&mut flash, [1; 8], values()).unwrap();
        let restarted: PersistedTable<[u8; 8], [u8; 12]> =
            PersistedTable::open(&mut flash, 4, "devices").unwrap();
        assert_eq!(restarted.len(), 1);
    }

    #[test]
    fn open_rejects_wrong_persist_id() {
        let mut flash = region();
        PersistedTable::<[u8; 8], [u8; 12]>::format(&mut flash, 4, "devices").unwrap();
        assert_eq!(
            PersistedTable::<[u8; 8], [u8; 12]>::open(&mut flash, 4, "other").err(),
            Some(Error::InvalidState)
        );
    }
}
