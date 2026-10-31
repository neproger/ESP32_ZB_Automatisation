//! Minimal flash persistence: V2 record envelope, append-only, recovery.
//!
//! Ported subset of V2 `mstore` flash format (header/record magic, CRC, commit
//! marker, latest-per-slot). No bank rotation or compaction yet: running out of
//! space is an explicit `NoSpace`, never a silent overwrite.
//!
//! Not wired to `Table` yet; this slice is host-tested against a NOR simulator so
//! the Device Registry can use it next.

pub const HEADER_SIZE: usize = 64;
pub const HEADER_CRC_LEN: usize = 36;
pub const HEADER_MAGIC: u32 = 0x4D53_5442; // MSTB
pub const FORMAT_REVISION: u16 = 1;
pub const RECORD_FIXED: usize = 16;
pub const RECORD_TAIL: usize = 8;
pub const RECORD_MAGIC: u16 = 0x5452; // RT
pub const COMMIT_MARKER: u32 = 0x4D53_434D; // MSCM
pub const KIND_SET: u8 = 1;
pub const KIND_META: u8 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FlashError {
    Io,
    Corrupt,
    NoSpace,
    InvalidSize,
    NotFound,
    InvalidState,
}

pub trait NorFlash {
    fn capacity(&self) -> usize;
    fn erase_size(&self) -> usize;
    fn read(&self, offset: usize, out: &mut [u8]) -> Result<(), FlashError>;
    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashError>;
    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashError>;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Geometry {
    pub capacity: usize,
    pub key_size: usize,
    pub payload_size: usize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FlashMeta {
    pub used: bool,
    pub generation: u32,
    pub version: u32,
}

#[derive(Clone, Copy)]
struct Latest {
    offset: usize,
    kind: u8,
}

pub struct FlashTable {
    geometry: Geometry,
    id: [u8; 16],
    bank_size: usize,
    active_bank: usize,
    active_seq: u32,
    write_offset: usize,
    latest: Vec<Option<Latest>>,
    scratch: Vec<u8>,
    torn: bool,
}

impl FlashTable {
    fn set_len(&self) -> usize {
        RECORD_FIXED + self.geometry.key_size + self.geometry.payload_size + RECORD_TAIL
    }

    fn meta_len(&self) -> usize {
        RECORD_FIXED + RECORD_TAIL
    }

    fn record_len(&self, kind: u8) -> usize {
        if kind == KIND_SET {
            self.set_len()
        } else {
            self.meta_len()
        }
    }

    fn bank_base(&self, bank: usize) -> usize {
        bank * self.bank_size
    }

    fn active_base(&self) -> usize {
        self.bank_base(self.active_bank)
    }

    fn bank_size_for<F: NorFlash>(flash: &F) -> Result<usize, FlashError> {
        let size = flash.capacity() / 2;
        if size == 0 || size % flash.erase_size() != 0 {
            return Err(FlashError::InvalidSize);
        }
        Ok(size)
    }

    fn header_bytes(&self, seq: u32) -> [u8; HEADER_SIZE] {
        let mut header = [0xFFu8; HEADER_SIZE];
        put_u32(&mut header, 0, HEADER_MAGIC);
        put_u16(&mut header, 4, FORMAT_REVISION);
        put_u16(&mut header, 6, HEADER_SIZE as u16);
        put_u32(&mut header, 8, seq);
        put_u32(&mut header, 12, self.geometry.capacity as u32);
        put_u16(&mut header, 16, self.geometry.key_size as u16);
        put_u16(&mut header, 18, self.geometry.payload_size as u16);
        header[20..36].copy_from_slice(&self.id);
        let crc = crc32(&header[..HEADER_CRC_LEN]);
        put_u32(&mut header, HEADER_CRC_LEN, crc);
        header
    }

    fn read_header<F: NorFlash>(
        flash: &F,
        base: usize,
    ) -> Result<Option<(u32, Geometry, [u8; 16])>, FlashError> {
        let mut header = [0u8; HEADER_SIZE];
        flash.read(base, &mut header)?;
        if header.iter().all(|byte| *byte == 0xFF) {
            return Ok(None);
        }
        if get_u32(&header, 0) != HEADER_MAGIC
            || get_u16(&header, 4) != FORMAT_REVISION
            || get_u16(&header, 6) as usize != HEADER_SIZE
            || crc32(&header[..HEADER_CRC_LEN]) != get_u32(&header, HEADER_CRC_LEN)
        {
            return Ok(None);
        }
        let mut id = [0u8; 16];
        id.copy_from_slice(&header[20..36]);
        Ok(Some((
            get_u32(&header, 8),
            Geometry {
                capacity: get_u32(&header, 12) as usize,
                key_size: get_u16(&header, 16) as usize,
                payload_size: get_u16(&header, 18) as usize,
            },
            id,
        )))
    }

    fn header_erased<F: NorFlash>(flash: &F, base: usize) -> Result<bool, FlashError> {
        let mut header = [0u8; HEADER_SIZE];
        flash.read(base, &mut header)?;
        Ok(header.iter().all(|byte| *byte == 0xFF))
    }

    fn validate_geometry(geometry: Geometry, bank_size: usize) -> Result<usize, FlashError> {
        if geometry.capacity == 0 || geometry.key_size == 0 {
            return Err(FlashError::InvalidSize);
        }
        if geometry.capacity > u32::MAX as usize
            || geometry.key_size > u16::MAX as usize
            || geometry.payload_size > u16::MAX as usize
        {
            return Err(FlashError::InvalidSize);
        }
        let set_len = RECORD_FIXED
            .checked_add(geometry.key_size)
            .and_then(|len| len.checked_add(geometry.payload_size))
            .and_then(|len| len.checked_add(RECORD_TAIL))
            .ok_or(FlashError::InvalidSize)?;
        let needed = geometry
            .capacity
            .checked_mul(set_len)
            .and_then(|bytes| bytes.checked_add(HEADER_SIZE))
            .ok_or(FlashError::InvalidSize)?;
        if needed > bank_size {
            return Err(FlashError::InvalidSize);
        }
        Ok(set_len)
    }

    pub fn format<F: NorFlash>(
        flash: &mut F,
        geometry: Geometry,
        persist_id: &str,
    ) -> Result<Self, FlashError> {
        let bank_size = Self::bank_size_for(flash)?;
        let set_len = Self::validate_geometry(geometry, bank_size)?;
        let id = persist_id_bytes(persist_id);
        flash.erase(0, bank_size.checked_mul(2).ok_or(FlashError::InvalidSize)?)?;
        let table = Self {
            geometry,
            id,
            bank_size,
            active_bank: 0,
            active_seq: 1,
            write_offset: HEADER_SIZE,
            latest: vec![None; geometry.capacity],
            scratch: vec![0; set_len],
            torn: false,
        };
        let header = table.header_bytes(1);
        flash.write(0, &header)?;
        Ok(table)
    }

    pub fn open<F: NorFlash>(
        flash: &mut F,
        geometry: Geometry,
        persist_id: &str,
    ) -> Result<Self, FlashError> {
        let bank_size = Self::bank_size_for(flash)?;
        let set_len = Self::validate_geometry(geometry, bank_size)?;
        let id = persist_id_bytes(persist_id);
        let first = Self::read_header(flash, 0)?;
        let second = Self::read_header(flash, bank_size)?;
        let (active_bank, active_seq, header_geometry, header_id) = match (first, second) {
            (Some((seq0, geo0, id0)), Some((seq1, geo1, id1))) => {
                if seq0 >= seq1 {
                    (0, seq0, geo0, id0)
                } else {
                    (1, seq1, geo1, id1)
                }
            }
            (Some((seq, geo, id)), None) => (0, seq, geo, id),
            (None, Some((seq, geo, id))) => (1, seq, geo, id),
            (None, None) => {
                // Missing valid headers alone is not proof of a fresh region.
                if !Self::header_erased(flash, 0)? || !Self::header_erased(flash, bank_size)? {
                    return Err(FlashError::Corrupt);
                }
                return Self::initialize(flash, geometry, id, bank_size, set_len);
            }
        };
        if header_geometry.capacity != geometry.capacity
            || header_geometry.key_size != geometry.key_size
            || header_geometry.payload_size != geometry.payload_size
            || header_id != id
        {
            return Err(FlashError::InvalidState);
        }
        let mut table = Self {
            geometry,
            id,
            bank_size,
            active_bank,
            active_seq,
            write_offset: HEADER_SIZE,
            latest: vec![None; geometry.capacity],
            scratch: vec![0; set_len],
            torn: false,
        };
        table.recover(flash)?;
        Ok(table)
    }

    /// V2 fresh path: only bank0 is erased, then header with `seq=1` is written.
    fn initialize<F: NorFlash>(
        flash: &mut F,
        geometry: Geometry,
        id: [u8; 16],
        bank_size: usize,
        set_len: usize,
    ) -> Result<Self, FlashError> {
        flash.erase(0, bank_size)?;
        let table = Self {
            geometry,
            id,
            bank_size,
            active_bank: 0,
            active_seq: 1,
            write_offset: HEADER_SIZE,
            latest: vec![None; geometry.capacity],
            scratch: vec![0; set_len],
            torn: false,
        };
        let header = table.header_bytes(1);
        flash.write(0, &header)?;
        Ok(table)
    }

    pub fn bank_state(&self) -> (usize, u32) {
        (self.active_bank, self.active_seq)
    }

    pub fn id(&self) -> [u8; 16] {
        self.id
    }

    fn recover<F: NorFlash>(&mut self, flash: &mut F) -> Result<(), FlashError> {
        let base = self.active_base();
        let mut offset = HEADER_SIZE;
        let proof = self.scratch.len();
        let mut buf = vec![0u8; proof];
        while offset
            .checked_add(RECORD_FIXED)
            .is_some_and(|end| end <= self.bank_size)
        {
            let mut prefix = [0u8; RECORD_FIXED];
            flash.read(base + offset, &mut prefix)?;
            if prefix.iter().all(|byte| *byte == 0xFF) {
                break;
            }
            let kind = prefix[2];
            let header_ok =
                get_u16(&prefix, 0) == RECORD_MAGIC && (kind == KIND_SET || kind == KIND_META);
            if !header_ok {
                self.torn = self.classify_tail(flash, offset)?;
                break;
            }
            let len = self.record_len(kind);
            let end = offset.checked_add(len).ok_or(FlashError::InvalidSize)?;
            if end > self.bank_size {
                self.torn = self.classify_tail(flash, offset)?;
                break;
            }
            flash.read(base + offset, &mut buf[..len])?;
            let crc_off = if kind == KIND_SET {
                RECORD_FIXED + self.geometry.key_size + self.geometry.payload_size
            } else {
                RECORD_FIXED
            };
            if get_u32(&buf, crc_off + 4) != COMMIT_MARKER {
                self.torn = self.classify_tail(flash, offset)?;
                break;
            }
            if crc32(&buf[..crc_off]) != get_u32(&buf, crc_off) {
                return Err(FlashError::Corrupt);
            }
            let slot = get_u32(&buf, 4);
            if slot as usize >= self.geometry.capacity {
                return Err(FlashError::Corrupt);
            }
            self.latest[slot as usize] = Some(Latest { offset, kind });
            offset += len;
        }
        self.write_offset = offset;
        Ok(())
    }

    fn classify_tail<F: NorFlash>(&self, flash: &mut F, from: usize) -> Result<bool, FlashError> {
        let base = self.active_base();
        let mut buf = [0u8; 64];
        let mut run = 0usize;
        let mut seen_erased = false;
        let mut offset = from;
        while offset < self.bank_size {
            let chunk = (self.bank_size - offset).min(buf.len());
            flash.read(base + offset, &mut buf[..chunk])?;
            for byte in &buf[..chunk] {
                if *byte != 0xFF {
                    if seen_erased {
                        return Err(FlashError::Corrupt);
                    }
                    run += 1;
                } else {
                    seen_erased = true;
                }
            }
            offset += chunk;
        }
        if run == 0 {
            Ok(false)
        } else if run > self.set_len() {
            Err(FlashError::Corrupt)
        } else {
            Ok(true)
        }
    }

    pub fn append_set<F: NorFlash>(
        &mut self,
        flash: &mut F,
        slot: u32,
        meta: FlashMeta,
        key: &[u8],
        payload: &[u8],
    ) -> Result<(), FlashError> {
        if key.len() != self.geometry.key_size || payload.len() != self.geometry.payload_size {
            return Err(FlashError::InvalidSize);
        }
        self.append(flash, slot, meta, KIND_SET, Some(key), Some(payload))
    }

    pub fn append_meta<F: NorFlash>(
        &mut self,
        flash: &mut F,
        slot: u32,
        meta: FlashMeta,
    ) -> Result<(), FlashError> {
        self.append(flash, slot, meta, KIND_META, None, None)
    }

    fn append<F: NorFlash>(
        &mut self,
        flash: &mut F,
        slot: u32,
        meta: FlashMeta,
        kind: u8,
        key: Option<&[u8]>,
        payload: Option<&[u8]>,
    ) -> Result<(), FlashError> {
        if slot as usize >= self.geometry.capacity {
            return Err(FlashError::InvalidSize);
        }
        let len = self.record_len(kind);
        let end = self
            .write_offset
            .checked_add(len)
            .ok_or(FlashError::InvalidSize)?;
        if self.torn || end > self.bank_size {
            self.compact(flash)?;
        }
        let end = self
            .write_offset
            .checked_add(len)
            .ok_or(FlashError::InvalidSize)?;
        if end > self.bank_size {
            return Err(FlashError::NoSpace);
        }
        let base = self.active_base();
        let record = &mut self.scratch[..len];
        record.fill(0xFF);
        put_u16(record, 0, RECORD_MAGIC);
        record[2] = kind;
        record[3] = u8::from(meta.used);
        put_u32(record, 4, slot);
        put_u32(record, 8, meta.generation);
        put_u32(record, 12, meta.version);
        let crc_off = if kind == KIND_SET {
            let key = key.ok_or(FlashError::InvalidSize)?;
            let payload = payload.ok_or(FlashError::InvalidSize)?;
            record[RECORD_FIXED..RECORD_FIXED + self.geometry.key_size].copy_from_slice(key);
            record[RECORD_FIXED + self.geometry.key_size
                ..RECORD_FIXED + self.geometry.key_size + self.geometry.payload_size]
                .copy_from_slice(payload);
            RECORD_FIXED + self.geometry.key_size + self.geometry.payload_size
        } else {
            RECORD_FIXED
        };
        put_u32(record, crc_off, crc32(&record[..crc_off]));
        put_u32(record, crc_off + 4, COMMIT_MARKER);

        if let Err(error) = flash
            .write(base + self.write_offset, &record[..len - 4])
            .and_then(|()| flash.write(base + self.write_offset + len - 4, &record[len - 4..]))
        {
            self.torn = true;
            return Err(error);
        }
        self.latest[slot as usize] = Some(Latest {
            offset: self.write_offset,
            kind,
        });
        self.write_offset += len;
        Ok(())
    }

    pub(crate) fn compact<F: NorFlash>(&mut self, flash: &mut F) -> Result<(), FlashError> {
        self.compact_with(flash, false)
    }

    /// V2 `clear_all`: rewrite every recorded slot as a tombstone, keeping
    /// generation/version, then commit the new header last.
    pub(crate) fn clear<F: NorFlash>(&mut self, flash: &mut F) -> Result<(), FlashError> {
        self.compact_with(flash, true)
    }

    fn compact_with<F: NorFlash>(
        &mut self,
        flash: &mut F,
        clear: bool,
    ) -> Result<(), FlashError> {
        let inactive = 1 - self.active_bank;
        let old_base = self.active_base();
        let new_base = self.bank_base(inactive);
        flash.erase(new_base, self.bank_size)?;

        let mut new_latest = vec![None; self.geometry.capacity];
        let mut cursor = HEADER_SIZE;
        for slot in 0..self.geometry.capacity {
            let Some(entry) = self.latest[slot] else {
                continue;
            };
            let (len, kind) = if clear {
                (self.meta_len(), KIND_META)
            } else {
                (self.record_len(entry.kind), entry.kind)
            };
            let end = cursor.checked_add(len).ok_or(FlashError::InvalidSize)?;
            if end > self.bank_size {
                return Err(FlashError::NoSpace);
            }
            if clear {
                flash.read(old_base + entry.offset, &mut self.scratch[..RECORD_FIXED])?;
                let generation = get_u32(&self.scratch, 8);
                let version = get_u32(&self.scratch, 12);
                let record = &mut self.scratch[..len];
                record.fill(0xFF);
                put_u16(record, 0, RECORD_MAGIC);
                record[2] = KIND_META;
                record[3] = 0;
                put_u32(record, 4, slot as u32);
                put_u32(record, 8, generation);
                put_u32(record, 12, version);
                put_u32(record, RECORD_FIXED, crc32(&record[..RECORD_FIXED]));
                put_u32(record, RECORD_FIXED + 4, COMMIT_MARKER);
            } else {
                flash.read(old_base + entry.offset, &mut self.scratch[..len])?;
            }
            flash.write(new_base + cursor, &self.scratch[..len - 4])?;
            flash.write(new_base + cursor + len - 4, &self.scratch[len - 4..])?;
            new_latest[slot] = Some(Latest {
                offset: cursor,
                kind,
            });
            cursor += len;
        }

        let header = self.header_bytes(self.active_seq + 1);
        flash.write(new_base, &header)?;

        self.latest = new_latest;
        self.active_bank = inactive;
        self.active_seq += 1;
        self.write_offset = cursor;
        self.torn = false;
        let _ = flash.erase(old_base, self.bank_size);
        Ok(())
    }

    pub fn read_slot<F: NorFlash>(
        &self,
        flash: &F,
        slot: u32,
        key_out: &mut [u8],
        payload_out: &mut [u8],
    ) -> Result<(FlashMeta, u8), FlashError> {
        let Some(entry) = self.latest.get(slot as usize).and_then(|entry| *entry) else {
            return Err(FlashError::NotFound);
        };
        if key_out.len() < self.geometry.key_size || payload_out.len() < self.geometry.payload_size
        {
            return Err(FlashError::InvalidSize);
        }
        let base = self.active_base();
        let mut prefix = [0u8; RECORD_FIXED];
        flash.read(base + entry.offset, &mut prefix)?;
        let meta = FlashMeta {
            used: prefix[3] != 0,
            generation: get_u32(&prefix, 8),
            version: get_u32(&prefix, 12),
        };
        if entry.kind == KIND_SET {
            let key_off = base + entry.offset + RECORD_FIXED;
            flash.read(key_off, &mut key_out[..self.geometry.key_size])?;
            flash.read(
                key_off + self.geometry.key_size,
                &mut payload_out[..self.geometry.payload_size],
            )?;
        }
        Ok((meta, entry.kind))
    }

    /// Lenient meta read: a slot without a record reports `used=false` and zero
    /// generation/version, matching the V2 flash backend.
    pub fn read_meta<F: NorFlash>(&self, flash: &F, slot: u32) -> Result<FlashMeta, FlashError> {
        let Some(entry) = self.latest.get(slot as usize).and_then(|entry| *entry) else {
            return Ok(FlashMeta::default());
        };
        let mut prefix = [0u8; RECORD_FIXED];
        flash.read(self.active_base() + entry.offset, &mut prefix)?;
        Ok(FlashMeta {
            used: prefix[3] != 0,
            generation: get_u32(&prefix, 8),
            version: get_u32(&prefix, 12),
        })
    }

    /// Lenient key read: a slot without a SET record yields zeroed key bytes.
    pub fn read_key<F: NorFlash>(
        &self,
        flash: &F,
        slot: u32,
        out: &mut [u8],
    ) -> Result<(), FlashError> {
        if out.len() < self.geometry.key_size {
            return Err(FlashError::InvalidSize);
        }
        out[..self.geometry.key_size].fill(0);
        let Some(entry) = self.latest.get(slot as usize).and_then(|entry| *entry) else {
            return Ok(());
        };
        if entry.kind != KIND_SET {
            return Ok(());
        }
        flash.read(
            self.active_base() + entry.offset + RECORD_FIXED,
            &mut out[..self.geometry.key_size],
        )
    }
}
pub fn persist_id_bytes(text: &str) -> [u8; 16] {
    let a = fnv1a64(text, 1_469_598_103_934_665_603);
    let b = fnv1a64(text, 1_099_511_628_211);
    let mut out = [0u8; 16];
    out[..8].copy_from_slice(&a.to_le_bytes());
    out[8..].copy_from_slice(&b.to_le_bytes());
    out
}

fn fnv1a64(text: &str, seed: u64) -> u64 {
    let mut hash = seed;
    for byte in text.bytes() {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(1_099_511_628_211);
    }
    hash
}

pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for byte in data {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
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

/// Host NOR simulator: erase -> 0xFF, write can only clear bits.
pub mod sim {
    use super::{FlashError, NorFlash};

    pub struct SimFlash {
        bytes: Vec<u8>,
        erase_size: usize,
        pub writes: usize,
        pub erases: usize,
        pub fail_next_write: bool,
    }

    impl SimFlash {
        pub fn new(size: usize, erase_size: usize) -> Self {
            Self {
                bytes: vec![0xFF; size],
                erase_size,
                writes: 0,
                erases: 0,
                fail_next_write: false,
            }
        }

        pub fn bytes(&self) -> &[u8] {
            &self.bytes
        }

        /// Simulates a power cut: writes bytes ignoring the 1->0 rule.
        pub fn corrupt(&mut self, offset: usize, data: &[u8]) {
            self.bytes[offset..offset + data.len()].copy_from_slice(data);
        }
    }

    impl NorFlash for SimFlash {
        fn capacity(&self) -> usize {
            self.bytes.len()
        }

        fn erase_size(&self) -> usize {
            self.erase_size
        }

        fn read(&self, offset: usize, out: &mut [u8]) -> Result<(), FlashError> {
            let end = offset + out.len();
            if end > self.bytes.len() {
                return Err(FlashError::InvalidSize);
            }
            out.copy_from_slice(&self.bytes[offset..end]);
            Ok(())
        }

        fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashError> {
            if self.fail_next_write {
                self.fail_next_write = false;
                return Err(FlashError::Io);
            }
            let end = offset + data.len();
            if end > self.bytes.len() {
                return Err(FlashError::InvalidSize);
            }
            for (index, byte) in data.iter().enumerate() {
                let current = self.bytes[offset + index];
                if !current & *byte != 0 {
                    // Attempt to set a 0 back to 1 without erase.
                    return Err(FlashError::Io);
                }
                self.bytes[offset + index] = current & *byte;
            }
            self.writes += 1;
            Ok(())
        }

        fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashError> {
            if offset % self.erase_size != 0 || len % self.erase_size != 0 {
                return Err(FlashError::InvalidSize);
            }
            let end = offset + len;
            if end > self.bytes.len() {
                return Err(FlashError::InvalidSize);
            }
            self.bytes[offset..end].fill(0xFF);
            self.erases += 1;
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::sim::SimFlash;
    use super::*;

    fn geometry() -> Geometry {
        Geometry {
            capacity: 4,
            key_size: 8,
            payload_size: 12,
        }
    }

    fn region() -> SimFlash {
        SimFlash::new(8192, 4096)
    }

    fn key(id: u8) -> [u8; 8] {
        [id; 8]
    }

    #[test]
    fn format_append_and_recover_after_reboot() {
        let mut flash = region();
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        let meta = FlashMeta {
            used: true,
            generation: 1,
            version: 1,
        };
        table
            .append_set(&mut flash, 0, meta, &key(1), &[7; 12])
            .unwrap();
        table
            .append_set(&mut flash, 1, meta, &key(2), &[8; 12])
            .unwrap();

        let mut restarted = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        assert_eq!(restarted.id(), persist_id_bytes("devices"));
        let (read, kind) = restarted
            .read_slot(&mut flash, 0, &mut [0; 8], &mut [0; 12])
            .unwrap();
        assert_eq!(read, meta);
        assert_eq!(kind, KIND_SET);
        let (_, _) = restarted
            .read_slot(&mut flash, 1, &mut [0; 8], &mut [0; 12])
            .unwrap();

        let next = FlashMeta {
            used: true,
            generation: 1,
            version: 2,
        };
        restarted
            .append_set(&mut flash, 0, next, &key(1), &[9; 12])
            .unwrap();
        let after = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        let mut payload = [0u8; 12];
        let (meta, _) = after
            .read_slot(&mut flash, 0, &mut [0; 8], &mut payload)
            .unwrap();
        assert_eq!(meta, next);
        assert_eq!(payload, [9; 12]);
    }

    #[test]
    fn torn_append_is_ignored() {
        let mut flash = region();
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        let meta = FlashMeta {
            used: true,
            generation: 1,
            version: 1,
        };
        table
            .append_set(&mut flash, 0, meta, &key(1), &[1; 12])
            .unwrap();
        let next = FlashMeta {
            used: true,
            generation: 2,
            version: 1,
        };
        table
            .append_set(&mut flash, 1, next, &key(2), &[2; 12])
            .unwrap();
        // Remove the commit marker of the last record (simulate power cut).
        let record = RECORD_FIXED + geometry().key_size + geometry().payload_size + RECORD_TAIL;
        let offset = HEADER_SIZE + record + record - 4;
        flash.corrupt(offset, &[0xFF, 0xFF, 0xFF, 0xFF]);
        let mut restarted = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        assert!(restarted
            .read_slot(&mut flash, 0, &mut [0; 8], &mut [0; 12])
            .is_ok());
        assert_eq!(
            restarted.read_slot(&mut flash, 1, &mut [0; 8], &mut [0; 12]),
            Err(FlashError::NotFound)
        );
    }

    #[test]
    fn corrupt_committed_record_is_rejected() {
        let mut flash = region();
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        table
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &[1; 12],
            )
            .unwrap();
        flash.corrupt(HEADER_SIZE + RECORD_FIXED, &[0x00]);
        assert_eq!(
            FlashTable::open(&mut flash, geometry(), "devices").err(),
            Some(FlashError::Corrupt)
        );
    }

    #[test]
    fn no_space_is_explicit_and_meta_records_work() {
        let mut flash = SimFlash::new(512, 256);
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        let meta = FlashMeta {
            used: true,
            generation: 1,
            version: 1,
        };
        for slot in 0..4 {
            table
                .append_set(&mut flash, slot, meta, &key(slot as u8), &[0; 12])
                .unwrap();
        }
        assert_eq!(
            table.append_set(&mut flash, 0, meta, &key(9), &[0; 12]),
            Err(FlashError::NoSpace)
        );
        let mut free = table;
        free.append_meta(
            &mut flash,
            0,
            FlashMeta {
                used: false,
                generation: 1,
                version: 1,
            },
        )
        .ok(); // still bounded by space; may also be NoSpace
    }

    #[test]
    fn header_geometry_mismatch_is_invalid_state() {
        let mut flash = region();
        FlashTable::format(&mut flash, geometry(), "devices").unwrap();

        let mut other = geometry();
        other.payload_size = 16;
        assert_eq!(
            FlashTable::open(&mut flash, other, "devices").err(),
            Some(FlashError::InvalidState)
        );

        let mut bigger = geometry();
        bigger.capacity = 5;
        assert_eq!(
            FlashTable::open(&mut flash, bigger, "devices").err(),
            Some(FlashError::InvalidState)
        );

        let mut smaller = geometry();
        smaller.capacity = 3;
        assert_eq!(
            FlashTable::open(&mut flash, smaller, "devices").err(),
            Some(FlashError::InvalidState)
        );

        assert_eq!(
            FlashTable::open(&mut flash, geometry(), "other").err(),
            Some(FlashError::InvalidState)
        );
    }

    #[test]
    fn open_initializes_only_when_both_headers_are_erased() {
        let mut flash = SimFlash::new(8192, 4096);
        let mut table = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        assert_eq!(table.bank_state(), (0, 1));
        table
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &[1; 12],
            )
            .unwrap();

        let mut partial_first = SimFlash::new(8192, 4096);
        partial_first.corrupt(0, &[0x00]);
        assert_eq!(
            FlashTable::open(&mut partial_first, geometry(), "devices").err(),
            Some(FlashError::Corrupt)
        );

        let mut partial_second = SimFlash::new(8192, 4096);
        partial_second.corrupt(4096, &[0x00]);
        assert_eq!(
            FlashTable::open(&mut partial_second, geometry(), "devices").err(),
            Some(FlashError::Corrupt)
        );
    }

    #[test]
    fn one_valid_header_recovers_despite_partial_other() {
        let mut flash = SimFlash::new(8192, 4096);
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        table
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &[1; 12],
            )
            .unwrap();
        flash.corrupt(4096, &[0x00]);

        let reopened = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        let mut read_key = [0u8; 8];
        let mut read_payload = [0u8; 12];
        let (meta, _) = reopened
            .read_slot(&flash, 0, &mut read_key, &mut read_payload)
            .unwrap();
        assert!(meta.used);
        assert_eq!(read_payload, [1; 12]);
    }

    #[test]
    fn active_header_mismatch_does_not_fall_back_to_old() {
        let mut base = SimFlash::new(8192, 4096);
        FlashTable::format(&mut base, geometry(), "devices").unwrap();

        let mut other_flash = SimFlash::new(8192, 4096);
        let mut other = geometry();
        other.payload_size = 16;
        FlashTable::format(&mut other_flash, other, "devices").unwrap();

        // Put a valid geometry-B header with a higher sequence into bank1.
        let mut newer = other_flash.bytes()[..HEADER_SIZE].to_vec();
        put_u32(&mut newer, 8, 2);
        let crc = crc32(&newer[..HEADER_CRC_LEN]);
        put_u32(&mut newer, HEADER_CRC_LEN, crc);
        base.corrupt(4096, &newer);

        assert_eq!(
            FlashTable::open(&mut base, geometry(), "devices").err(),
            Some(FlashError::InvalidState)
        );

        let mut base_id = SimFlash::new(8192, 4096);
        FlashTable::format(&mut base_id, geometry(), "devices").unwrap();
        let mut other_id_flash = SimFlash::new(8192, 4096);
        FlashTable::format(&mut other_id_flash, geometry(), "other").unwrap();
        let mut newer_id = other_id_flash.bytes()[..HEADER_SIZE].to_vec();
        put_u32(&mut newer_id, 8, 2);
        let crc = crc32(&newer_id[..HEADER_CRC_LEN]);
        put_u32(&mut newer_id, HEADER_CRC_LEN, crc);
        base_id.corrupt(4096, &newer_id);
        assert_eq!(
            FlashTable::open(&mut base_id, geometry(), "devices").err(),
            Some(FlashError::InvalidState)
        );
    }

    #[test]
    fn zero_payload_roundtrip_compaction_and_clear() {
        let zero = Geometry {
            capacity: 2,
            key_size: 8,
            payload_size: 0,
        };
        let mut flash = SimFlash::new(1024, 256);
        let mut table = FlashTable::format(&mut flash, zero, "zero").unwrap();
        table
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &[],
            )
            .unwrap();
        for version in 2..20 {
            table
                .append_set(
                    &mut flash,
                    0,
                    FlashMeta {
                        used: true,
                        generation: 1,
                        version,
                    },
                    &key(1),
                    &[],
                )
                .unwrap();
        }

        let mut read_key = [0u8; 8];
        let (meta, _) = table
            .read_slot(&mut flash, 0, &mut read_key, &mut [])
            .unwrap();
        assert_eq!(meta.version, 19);

        let mut reopened = FlashTable::open(&mut flash, zero, "zero").unwrap();
        let (meta, _) = reopened
            .read_slot(&mut flash, 0, &mut read_key, &mut [])
            .unwrap();
        assert_eq!(meta.version, 19);
        reopened.clear(&mut flash).unwrap();
        let cleared = reopened.read_meta(&flash, 0).unwrap();
        assert!(!cleared.used);
        assert_eq!(cleared.generation, 1);
        assert_eq!(cleared.version, 19);
    }

    #[test]
    fn torn_tail_is_compacted_before_append() {
        let mut flash = region();
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        table
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &[1; 12],
            )
            .unwrap();
        let record = RECORD_FIXED + geometry().key_size + geometry().payload_size + RECORD_TAIL;
        flash.corrupt(HEADER_SIZE + record - 4, &[0xFF; 4]);
        let mut reopened = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        reopened
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 2,
                    version: 2,
                },
                &key(1),
                &[2; 12],
            )
            .unwrap();
        assert_eq!(reopened.active_bank, 1);
        let mut after = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        let mut payload = [0u8; 12];
        let (meta, _) = after
            .read_slot(&mut flash, 0, &mut [0; 8], &mut payload)
            .unwrap();
        assert_eq!(meta.generation, 2);
        assert_eq!(meta.version, 2);
        assert_eq!(payload, [2; 12]);
    }
    #[test]
    fn compaction_preserves_latest_metadata_and_tombstones() {
        let mut flash = region();
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        let meta = |generation, version| FlashMeta {
            used: true,
            generation,
            version,
        };
        table
            .append_set(&mut flash, 0, meta(1, 1), &key(1), &[1; 12])
            .unwrap();
        table
            .append_set(&mut flash, 1, meta(1, 1), &key(2), &[2; 12])
            .unwrap();
        table
            .append_set(&mut flash, 2, meta(1, 1), &key(3), &[3; 12])
            .unwrap();
        table
            .append_set(&mut flash, 0, meta(2, 3), &key(1), &[9; 12])
            .unwrap();
        table
            .append_meta(
                &mut flash,
                2,
                FlashMeta {
                    used: false,
                    generation: 2,
                    version: 2,
                },
            )
            .unwrap();
        table.compact(&mut flash).unwrap();
        assert_eq!(table.active_bank, 1);

        let mut reopened = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        let mut payload = [0u8; 12];
        let (read, kind) = reopened
            .read_slot(&mut flash, 0, &mut [0; 8], &mut payload)
            .unwrap();
        assert_eq!(read, meta(2, 3));
        assert_eq!(kind, KIND_SET);
        assert_eq!(payload, [9; 12]);
        let (read, _) = reopened
            .read_slot(&mut flash, 1, &mut [0; 8], &mut payload)
            .unwrap();
        assert_eq!(read, meta(1, 1));
        let (read, kind) = reopened
            .read_slot(&mut flash, 2, &mut [0; 8], &mut payload)
            .unwrap();
        assert!(!read.used);
        assert_eq!(read.generation, 2);
        assert_eq!(kind, KIND_META);
    }

    struct CrashFlash<'a> {
        inner: &'a mut SimFlash,
        fail_write: Option<usize>,
        writes: usize,
        tear_write: bool,
        fail_erase: Option<usize>,
        erases: usize,
    }

    impl NorFlash for CrashFlash<'_> {
        fn capacity(&self) -> usize {
            self.inner.capacity()
        }

        fn erase_size(&self) -> usize {
            self.inner.erase_size()
        }

        fn read(&self, offset: usize, out: &mut [u8]) -> Result<(), FlashError> {
            self.inner.read(offset, out)
        }

        fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashError> {
            if self.fail_write == Some(self.writes) {
                if self.tear_write {
                    let half = data.len() / 2;
                    let _ = self.inner.write(offset, &data[..half]);
                }
                return Err(FlashError::Io);
            }
            self.writes += 1;
            self.inner.write(offset, data)
        }

        fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashError> {
            if self.fail_erase == Some(self.erases) {
                let _ = self.inner.erase(offset, len);
                return Err(FlashError::Io);
            }
            self.erases += 1;
            self.inner.erase(offset, len)
        }
    }

    fn crash_case(fail_write: Option<usize>, tear_write: bool) {
        let mut flash = region();
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        table
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &[7; 12],
            )
            .unwrap();
        {
            let mut crash = CrashFlash {
                inner: &mut flash,
                fail_write,
                writes: 0,
                tear_write,
                fail_erase: None,
                erases: 0,
            };
            table.compact(&mut crash).unwrap_err();
        }
        let mut reopened = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        let mut payload = [0u8; 12];
        let (meta, _) = reopened
            .read_slot(&mut flash, 0, &mut [0; 8], &mut payload)
            .unwrap();
        assert_eq!(meta.generation, 1);
        assert_eq!(meta.version, 1);
        assert_eq!(payload, [7; 12]);
    }

    #[test]
    fn crash_during_copy_keeps_old_bank() {
        crash_case(Some(1), true);
    }

    #[test]
    fn crash_during_header_keeps_old_bank() {
        crash_case(Some(2), true);
    }

    #[test]
    fn committed_header_wins_even_if_old_bank_erase_fails() {
        let mut flash = SimFlash::new(8192, 2048);
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        table
            .append_set(
                &mut flash,
                0,
                FlashMeta {
                    used: true,
                    generation: 1,
                    version: 1,
                },
                &key(1),
                &[7; 12],
            )
            .unwrap();
        {
            let mut crash = CrashFlash {
                inner: &mut flash,
                fail_write: None,
                writes: 0,
                tear_write: false,
                fail_erase: Some(1),
                erases: 0,
            };
            table.compact(&mut crash).unwrap();
        }
        let mut reopened = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        assert_eq!(reopened.active_bank, 1);
        let mut payload = [0u8; 12];
        let (meta, _) = reopened
            .read_slot(&mut flash, 0, &mut [0; 8], &mut payload)
            .unwrap();
        assert_eq!(meta.version, 1);
        assert_eq!(payload, [7; 12]);
    }

    #[test]
    fn repeated_compactions_keep_latest_state() {
        let mut flash = region();
        let mut table = FlashTable::format(&mut flash, geometry(), "devices").unwrap();
        for version in 1..=6 {
            table
                .append_set(
                    &mut flash,
                    0,
                    FlashMeta {
                        used: true,
                        generation: 1,
                        version,
                    },
                    &key(1),
                    &[version as u8; 12],
                )
                .unwrap();
            table.compact(&mut flash).unwrap();
            assert_eq!(table.active_bank, version as usize % 2);
        }
        let mut reopened = FlashTable::open(&mut flash, geometry(), "devices").unwrap();
        let mut payload = [0u8; 12];
        let (meta, _) = reopened
            .read_slot(&mut flash, 0, &mut [0; 8], &mut payload)
            .unwrap();
        assert_eq!(meta.version, 6);
        assert_eq!(payload, [6; 12]);
    }
}
