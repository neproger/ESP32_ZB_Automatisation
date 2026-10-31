//! Internal storage seam between the Table Engine and physical slot storage.
//!
//! Contract follows V2 `mstore_storage.h`: the engine owns slot semantics
//! (identity, generation/version, index), storage owns canonical slot bytes
//! (`meta + key + payload`). Reads take `&self`, writes take `&mut self`.
//!
//! These types are public only as a cross-crate seam for the composition root
//! (`ha-app`); services must not use them directly.

use crate::flash::{FlashError, FlashMeta, FlashTable, KIND_SET};
use crate::{Error, Geometry, Meta, NorFlash, Result};

const RAM_META_LEN: usize = 12;

pub trait Storage {
    fn read_meta(&self, slot: u32) -> Result<Meta>;
    fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<()>;
    fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta>;

    fn write_slot(&mut self, slot: u32, meta: Meta, key: &[u8], payload: &[u8]) -> Result<()>;
    fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<()>;
    fn clear_all(&mut self) -> Result<()>;
    fn sync(&mut self) -> Result<()>;
}

fn to_meta(meta: FlashMeta) -> Meta {
    Meta {
        used: meta.used,
        generation: meta.generation,
        version: meta.version,
    }
}

fn to_flash_meta(meta: Meta) -> FlashMeta {
    FlashMeta {
        used: meta.used,
        generation: meta.generation,
        version: meta.version,
    }
}

/// Flash-backed storage: one region/partition, format owned by `FlashTable`.
pub struct FlashStorage<F: NorFlash> {
    flash: F,
    table: FlashTable,
    geometry: Geometry,
}

impl<F: NorFlash> FlashStorage<F> {
    pub fn open(flash: F, geometry: Geometry, persist_id: &str) -> Result<Self> {
        let mut flash = flash;
        let table = FlashTable::open(&mut flash, geometry, persist_id)?;
        Ok(Self {
            flash,
            table,
            geometry,
        })
    }

    pub fn format(flash: F, geometry: Geometry, persist_id: &str) -> Result<Self> {
        let mut flash = flash;
        let table = FlashTable::format(&mut flash, geometry, persist_id)?;
        Ok(Self {
            flash,
            table,
            geometry,
        })
    }

    /// Returns the flash device so the caller can reopen the storage.
    pub fn into_flash(self) -> F {
        self.flash
    }

    fn check_slot(&self, slot: u32) -> Result<()> {
        if slot as usize >= self.geometry.capacity {
            return Err(Error::InvalidArg);
        }
        Ok(())
    }

    fn check_buffers(&self, key: &[u8], payload: &[u8]) -> Result<()> {
        if key.len() < self.geometry.key_size || payload.len() < self.geometry.payload_size {
            return Err(Error::InvalidSize);
        }
        Ok(())
    }
}

impl<F: NorFlash> Storage for FlashStorage<F> {
    fn read_meta(&self, slot: u32) -> Result<Meta> {
        self.check_slot(slot)?;
        self.table
            .read_meta(&self.flash, slot)
            .map(to_meta)
            .map_err(Error::from)
    }

    fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<()> {
        self.check_slot(slot)?;
        if out.len() < self.geometry.key_size {
            return Err(Error::InvalidSize);
        }
        self.table
            .read_key(&self.flash, slot, out)
            .map_err(Error::from)
    }

    fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta> {
        self.check_slot(slot)?;
        self.check_buffers(key, payload)?;
        let (meta, kind) = match self.table.read_slot(&self.flash, slot, key, payload) {
            Ok(value) => value,
            Err(FlashError::NotFound) => {
                key[..self.geometry.key_size].fill(0);
                payload[..self.geometry.payload_size].fill(0);
                return Ok(Meta::default());
            }
            Err(error) => return Err(error.into()),
        };
        if kind != KIND_SET {
            key[..self.geometry.key_size].fill(0);
            payload[..self.geometry.payload_size].fill(0);
        }
        Ok(to_meta(meta))
    }

    fn write_slot(&mut self, slot: u32, meta: Meta, key: &[u8], payload: &[u8]) -> Result<()> {
        self.check_slot(slot)?;
        self.table
            .append_set(&mut self.flash, slot, to_flash_meta(meta), key, payload)
            .map_err(Error::from)
    }

    fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<()> {
        self.check_slot(slot)?;
        self.table
            .append_meta(&mut self.flash, slot, to_flash_meta(meta))
            .map_err(Error::from)
    }

    fn clear_all(&mut self) -> Result<()> {
        self.table.clear(&mut self.flash).map_err(Error::from)
    }

    fn sync(&mut self) -> Result<()> {
        Ok(())
    }
}

/// RAM-backed storage: contiguous slot bytes with the V2 RAM clear semantics.
pub struct RamStorage {
    geometry: Geometry,
    slots: Vec<u8>,
}

impl RamStorage {
    pub fn new(geometry: Geometry) -> Result<Self> {
        if geometry.capacity == 0 || geometry.key_size == 0 {
            return Err(Error::InvalidSize);
        }
        let slot_len = Self::slot_len(&geometry)?;
        let bytes = geometry
            .capacity
            .checked_mul(slot_len)
            .ok_or(Error::InvalidSize)?;
        Ok(Self {
            geometry,
            slots: vec![0u8; bytes],
        })
    }

    fn slot_len(geometry: &Geometry) -> Result<usize> {
        RAM_META_LEN
            .checked_add(geometry.key_size)
            .and_then(|len| len.checked_add(geometry.payload_size))
            .ok_or(Error::InvalidSize)
    }

    fn offset(&self, slot: u32) -> Result<usize> {
        if slot as usize >= self.geometry.capacity {
            return Err(Error::InvalidArg);
        }
        Ok(slot as usize * Self::slot_len(&self.geometry)?)
    }

    fn check_buffers(&self, key: &[u8], payload: &[u8]) -> Result<()> {
        if key.len() < self.geometry.key_size || payload.len() < self.geometry.payload_size {
            return Err(Error::InvalidSize);
        }
        Ok(())
    }
}

impl Storage for RamStorage {
    fn read_meta(&self, slot: u32) -> Result<Meta> {
        let offset = self.offset(slot)?;
        let slot = &self.slots[offset..offset + Self::slot_len(&self.geometry)?];
        Ok(Meta {
            used: slot[0] != 0,
            generation: get_u32(slot, 4),
            version: get_u32(slot, 8),
        })
    }

    fn read_key(&self, slot: u32, out: &mut [u8]) -> Result<()> {
        let offset = self.offset(slot)?;
        if out.len() < self.geometry.key_size {
            return Err(Error::InvalidSize);
        }
        let slot = &self.slots[offset..offset + Self::slot_len(&self.geometry)?];
        if slot[0] == 0 {
            out[..self.geometry.key_size].fill(0);
        } else {
            out[..self.geometry.key_size]
                .copy_from_slice(&slot[RAM_META_LEN..RAM_META_LEN + self.geometry.key_size]);
        }
        Ok(())
    }

    fn read_slot(&self, slot: u32, key: &mut [u8], payload: &mut [u8]) -> Result<Meta> {
        let offset = self.offset(slot)?;
        self.check_buffers(key, payload)?;
        let bytes = &self.slots[offset..offset + Self::slot_len(&self.geometry)?];
        let meta = Meta {
            used: bytes[0] != 0,
            generation: get_u32(bytes, 4),
            version: get_u32(bytes, 8),
        };
        if meta.used {
            key[..self.geometry.key_size]
                .copy_from_slice(&bytes[RAM_META_LEN..RAM_META_LEN + self.geometry.key_size]);
            let payload_at = RAM_META_LEN + self.geometry.key_size;
            payload[..self.geometry.payload_size].copy_from_slice(
                &bytes[payload_at..payload_at + self.geometry.payload_size],
            );
        } else {
            key[..self.geometry.key_size].fill(0);
            payload[..self.geometry.payload_size].fill(0);
        }
        Ok(meta)
    }

    fn write_slot(&mut self, slot: u32, meta: Meta, key: &[u8], payload: &[u8]) -> Result<()> {
        let offset = self.offset(slot)?;
        if key.len() != self.geometry.key_size || payload.len() != self.geometry.payload_size {
            return Err(Error::InvalidSize);
        }
        let len = Self::slot_len(&self.geometry)?;
        let bytes = &mut self.slots[offset..offset + len];
        bytes.fill(0);
        write_meta_bytes(bytes, meta);
        bytes[RAM_META_LEN..RAM_META_LEN + self.geometry.key_size].copy_from_slice(key);
        let payload_at = RAM_META_LEN + self.geometry.key_size;
        bytes[payload_at..payload_at + self.geometry.payload_size].copy_from_slice(payload);
        Ok(())
    }

    fn write_meta(&mut self, slot: u32, meta: Meta) -> Result<()> {
        let offset = self.offset(slot)?;
        let len = Self::slot_len(&self.geometry)?;
        write_meta_bytes(&mut self.slots[offset..offset + len], meta);
        Ok(())
    }

    fn clear_all(&mut self) -> Result<()> {
        let len = Self::slot_len(&self.geometry)?;
        for slot in 0..self.geometry.capacity {
            self.slots[slot * len] = 0;
        }
        Ok(())
    }

    fn sync(&mut self) -> Result<()> {
        Ok(())
    }
}

fn write_meta_bytes(out: &mut [u8], meta: Meta) {
    out[0] = u8::from(meta.used);
    out[1..4].fill(0);
    put_u32(out, 4, meta.generation);
    put_u32(out, 8, meta.version);
}

fn put_u32(buf: &mut [u8], offset: usize, value: u32) {
    buf[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
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

    fn geometry() -> Geometry {
        Geometry {
            capacity: 4,
            key_size: 8,
            payload_size: 12,
        }
    }

    fn key(id: u8) -> [u8; 8] {
        [id; 8]
    }

    fn payload(id: u8) -> [u8; 12] {
        [id; 12]
    }

    fn read_via_shared<F: NorFlash>(flash: &F) -> core::result::Result<(), FlashError> {
        let mut out = [0u8; 4];
        flash.read(0, &mut out)
    }

    fn meta_via_shared<S: Storage>(storage: &S, slot: u32) -> Result<Meta> {
        storage.read_meta(slot)
    }

    #[test]
    fn nor_flash_reads_take_shared_ref() {
        let flash = SimFlash::new(1024, 256);
        read_via_shared(&flash).unwrap();
    }

    #[test]
    fn ram_storage_roundtrip_and_lenient_free_slot() {
        let mut storage = RamStorage::new(geometry()).unwrap();
        let meta = Meta {
            used: true,
            generation: 3,
            version: 2,
        };
        storage
            .write_slot(0, meta, &key(1), &payload(7))
            .unwrap();

        let mut read_key = [0u8; 8];
        let mut read_payload = [0u8; 12];
        let read_meta = storage.read_slot(0, &mut read_key, &mut read_payload).unwrap();
        assert_eq!(read_meta, meta);
        assert_eq!(read_key, key(1));
        assert_eq!(read_payload, payload(7));
        assert_eq!(meta_via_shared(&storage, 0).unwrap(), meta);

        let missing = storage.read_slot(2, &mut read_key, &mut read_payload).unwrap();
        assert_eq!(missing, Meta::default());
        assert_eq!(read_key, [0u8; 8]);
        assert_eq!(read_payload, [0u8; 12]);

        assert_eq!(storage.read_meta(4), Err(Error::InvalidArg));
        assert_eq!(
            storage.write_slot(0, meta, &[0u8; 7], &payload(0)),
            Err(Error::InvalidSize)
        );
    }

    #[test]
    fn ram_storage_meta_does_not_touch_key_or_payload() {
        let mut storage = RamStorage::new(geometry()).unwrap();
        storage
            .write_slot(
                1,
                Meta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(4),
                &payload(5),
            )
            .unwrap();
        storage
            .write_meta(
                1,
                Meta {
                    used: true,
                    generation: 1,
                    version: 9,
                },
            )
            .unwrap();

        let mut read_key = [0u8; 8];
        let mut read_payload = [0u8; 12];
        let meta = storage.read_slot(1, &mut read_key, &mut read_payload).unwrap();
        assert_eq!(meta.version, 9);
        assert_eq!(read_key, key(4));
        assert_eq!(read_payload, payload(5));
    }

    #[test]
    fn ram_storage_clear_preserves_generation() {
        let mut storage = RamStorage::new(geometry()).unwrap();
        let meta = Meta {
            used: true,
            generation: 6,
            version: 4,
        };
        storage.write_slot(0, meta, &key(1), &payload(2)).unwrap();
        storage.clear_all().unwrap();

        let read = storage.read_meta(0).unwrap();
        assert!(!read.used);
        assert_eq!(read.generation, 6);
        assert_eq!(read.version, 4);

        let mut read_key = [0u8; 8];
        let mut read_payload = [0u8; 12];
        storage.read_slot(0, &mut read_key, &mut read_payload).unwrap();
        assert_eq!(read_key, [0u8; 8]);
        assert_eq!(read_payload, [0u8; 12]);
    }

    #[test]
    fn flash_storage_roundtrip_and_persist_id() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        let meta = Meta {
            used: true,
            generation: 2,
            version: 5,
        };
        storage
            .write_slot(1, meta, &key(3), &payload(9))
            .unwrap();

        let flash = storage.into_flash();
        assert_eq!(
            FlashStorage::open(flash, geometry(), "other").err(),
            Some(Error::InvalidState)
        );
    }

    #[test]
    fn flash_storage_reopens_and_reads_lenient_slots() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        let meta = Meta {
            used: true,
            generation: 2,
            version: 5,
        };
        storage
            .write_slot(1, meta, &key(3), &payload(9))
            .unwrap();
        let flash = storage.into_flash();

        let storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        let mut read_key = [0u8; 8];
        let mut read_payload = [0u8; 12];
        let read = storage
            .read_slot(1, &mut read_key, &mut read_payload)
            .unwrap();
        assert_eq!(read, meta);
        assert_eq!(read_key, key(3));
        assert_eq!(read_payload, payload(9));

        let missing = storage
            .read_slot(3, &mut read_key, &mut read_payload)
            .unwrap();
        assert_eq!(missing, Meta::default());
        assert_eq!(read_key, [0u8; 8]);
        assert_eq!(read_payload, [0u8; 12]);
        assert_eq!(storage.read_meta(3).unwrap(), Meta::default());
        assert_eq!(
            storage.read_meta(4),
            Err(Error::InvalidArg)
        );
    }

    #[test]
    fn flash_storage_tombstone_keeps_meta_and_zeros_key() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        storage
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 4,
                    version: 7,
                },
                &key(2),
                &payload(2),
            )
            .unwrap();
        storage
            .write_meta(
                0,
                Meta {
                    used: false,
                    generation: 4,
                    version: 7,
                },
            )
            .unwrap();

        let meta = storage.read_meta(0).unwrap();
        assert!(!meta.used);
        assert_eq!(meta.generation, 4);
        assert_eq!(meta.version, 7);

        let mut read_key = [0u8; 8];
        let mut read_payload = [0u8; 12];
        storage.read_slot(0, &mut read_key, &mut read_payload).unwrap();
        assert_eq!(read_key, [0u8; 8]);
        assert_eq!(read_payload, [0u8; 12]);
    }

    #[test]
    fn flash_storage_clear_is_durable_and_keeps_generation() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        storage
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 5,
                    version: 3,
                },
                &key(1),
                &payload(1),
            )
            .unwrap();
        storage.clear_all().unwrap();

        let flash = storage.into_flash();
        let storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        let meta = storage.read_meta(0).unwrap();
        assert!(!meta.used);
        assert_eq!(meta.generation, 5);
        assert_eq!(meta.version, 3);
    }

    #[test]
    fn flash_storage_write_failure_keeps_previous_state() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        storage
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &payload(1),
            )
            .unwrap();

        let mut flash = storage.into_flash();
        flash.fail_next_write = true;
        let mut storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        assert_eq!(
            storage.write_slot(
                1,
                Meta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(2),
                &payload(2),
            ),
            Err(Error::Io)
        );

        let flash = storage.into_flash();
        let storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        assert!(storage.read_meta(0).unwrap().used);
        assert!(!storage.read_meta(1).unwrap().used);
    }

    #[test]
    fn flash_storage_clear_failure_keeps_previous_state() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        storage
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 2,
                    version: 2,
                },
                &key(4),
                &payload(4),
            )
            .unwrap();

        let mut flash = storage.into_flash();
        flash.fail_next_write = true;
        let mut storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        assert_eq!(storage.clear_all(), Err(Error::Io));

        let flash = storage.into_flash();
        let storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        let meta = storage.read_meta(0).unwrap();
        assert!(meta.used);
        assert_eq!(meta.generation, 2);
        assert_eq!(meta.version, 2);
    }

    #[test]
    fn flash_storage_clear_preserves_tombstone_meta() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        storage
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 8,
                    version: 2,
                },
                &key(3),
                &payload(3),
            )
            .unwrap();
        storage
            .write_meta(
                0,
                Meta {
                    used: false,
                    generation: 8,
                    version: 2,
                },
            )
            .unwrap();
        storage.clear_all().unwrap();

        let flash = storage.into_flash();
        let storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        let meta = storage.read_meta(0).unwrap();
        assert!(!meta.used);
        assert_eq!(meta.generation, 8);
        assert_eq!(meta.version, 2);
    }

    struct FailAt<'a> {
        inner: &'a mut SimFlash,
        fail_at: usize,
        writes: usize,
    }

    impl NorFlash for FailAt<'_> {
        fn capacity(&self) -> usize {
            self.inner.capacity()
        }

        fn erase_size(&self) -> usize {
            self.inner.erase_size()
        }

        fn read(&self, offset: usize, out: &mut [u8]) -> core::result::Result<(), FlashError> {
            self.inner.read(offset, out)
        }

        fn write(&mut self, offset: usize, data: &[u8]) -> core::result::Result<(), FlashError> {
            if self.writes == self.fail_at {
                return Err(FlashError::Io);
            }
            self.writes += 1;
            self.inner.write(offset, data)
        }

        fn erase(&mut self, offset: usize, len: usize) -> core::result::Result<(), FlashError> {
            self.inner.erase(offset, len)
        }
    }

    #[test]
    fn flash_storage_clear_header_failure_keeps_previous_state() {
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        storage
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 3,
                    version: 1,
                },
                &key(5),
                &payload(5),
            )
            .unwrap();
        let mut flash = storage.into_flash();

        {
            // One SET record is two programs; the third write is the new header.
            let failing = FailAt {
                inner: &mut flash,
                fail_at: 2,
                writes: 0,
            };
            let mut storage = FlashStorage::open(failing, geometry(), "devices").unwrap();
            assert_eq!(storage.clear_all(), Err(Error::Io));
        }

        let storage = FlashStorage::open(flash, geometry(), "devices").unwrap();
        let meta = storage.read_meta(0).unwrap();
        assert!(meta.used);
        assert_eq!(meta.generation, 3);
        assert_eq!(meta.version, 1);
    }

    #[test]
    fn flash_storage_open_initializes_erased_partition() {
        let storage =
            FlashStorage::open(SimFlash::new(8192, 1024), geometry(), "devices").unwrap();
        assert_eq!(storage.read_meta(0).unwrap(), Meta::default());
    }

    #[test]
    fn flash_storage_zero_payload_roundtrip() {
        let zero = Geometry {
            capacity: 2,
            key_size: 8,
            payload_size: 0,
        };
        let mut storage =
            FlashStorage::format(SimFlash::new(8192, 1024), zero, "zero").unwrap();
        storage
            .write_slot(
                0,
                Meta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(6),
                &[],
            )
            .unwrap();

        let mut read_key = [0u8; 8];
        let meta = storage.read_slot(0, &mut read_key, &mut []).unwrap();
        assert!(meta.used);
        assert_eq!(read_key, key(6));

        let flash = storage.into_flash();
        let storage = FlashStorage::open(flash, zero, "zero").unwrap();
        let meta = storage.read_slot(0, &mut read_key, &mut []).unwrap();
        assert!(meta.used);
    }
}
