//! Binary web protocol, v3 canonical frames (V2 `WEB_PROTOCOL.md`).
//!
//! The transport only frames bytes: `ver/type/len/seq/flags` plus a canonical
//! payload. Interpretation stays in the browser, so no DTOs exist here either.
//! `seq` belongs to a client session and is assigned by the sender.

pub const VER: u8 = 3;

/// Control frame types (outside the `ha_entity_t` range).
pub mod control {
    pub const SYNC_BEGIN: u8 = 0x01;
    pub const SYNC_END: u8 = 0x02;
    pub const EVENT: u8 = 0xE0;
    pub const COMMAND: u8 = 0x20;
}

/// Entity frame types; values are the V2 canonical ABI and must not change.
pub mod entity {
    pub const ENTITY: u8 = 14;
    pub const PROPERTY_STATE: u8 = 15;
    pub const LOGICAL_DEVICE: u8 = 17;
}

/// Browser -> device command ids used by the current slice.
pub mod command {
    pub const SNAPSHOT: u8 = 1;
}

pub const HEADER_LEN: usize = 8;

/// Appends one frame to `out`; `seq` is per session.
pub fn write_frame(out: &mut Vec<u8>, seq: u16, frame_type: u8, payload: &[u8]) {
    let len = u16::try_from(payload.len()).expect("frame payload fits u16");
    out.push(VER);
    out.push(frame_type);
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(&seq.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(payload);
}

/// `u32 count` payload of SYNC_BEGIN / SYNC_END.
pub fn sync_payload(count: u32) -> [u8; 4] {
    count.to_le_bytes()
}

/// Logical device record (80 bytes): parent u64, source u32, native_id[32], name[32], reserved[4].
pub fn logical_device(parent: u64, source: u32, native_id: &str, name: &str) -> [u8; 80] {
    let mut record = [0u8; 80];
    record[0..8].copy_from_slice(&parent.to_le_bytes());
    record[8..12].copy_from_slice(&source.to_le_bytes());
    copy_str(&mut record[12..44], native_id);
    copy_str(&mut record[44..76], name);
    record
}

/// Entity record (40 bytes): device_id u64, name[32].
pub fn entity(device_id: u64, name: &str) -> [u8; 40] {
    let mut record = [0u8; 40];
    record[0..8].copy_from_slice(&device_id.to_le_bytes());
    copy_str(&mut record[8..40], name);
    record
}

/// Entity key: 8-byte device id.
pub fn entity_key(id: u64) -> [u8; 8] {
    id.to_le_bytes()
}

fn copy_str(target: &mut [u8], value: &str) {
    let bytes = value.as_bytes();
    let len = bytes.len().min(target.len() - 1); // keep it NUL-terminated
    target[..len].copy_from_slice(&bytes[..len]);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_is_little_endian_and_len_excludes_header() {
        let mut out = Vec::new();
        write_frame(&mut out, 7, control::SYNC_BEGIN, &sync_payload(1));
        assert_eq!(&out[..8], &[3, 0x01, 4, 0, 7, 0, 0, 0]);
        assert_eq!(&out[8..], &[1, 0, 0, 0]);
        assert_eq!(out.len(), HEADER_LEN + 4);
    }

    #[test]
    fn logical_device_layout_matches_v2() {
        let record = logical_device(0, 1, "TS0041", "Button");
        assert_eq!(&record[0..8], &0u64.to_le_bytes());
        assert_eq!(&record[8..12], &1u32.to_le_bytes());
        assert_eq!(&record[12..18], b"TS0041");
        assert_eq!(record[18], 0);
        assert_eq!(&record[44..50], b"Button");
        assert_eq!(record.len(), 80);
    }

    #[test]
    fn entity_layout_and_key() {
        let record = entity(0xa4c1_383b_f9d6_2a03, "TS0041 button");
        assert_eq!(&record[0..8], &0xa4c1_383b_f9d6_2a03u64.to_le_bytes());
        assert_eq!(&record[8..21], b"TS0041 button");
        assert_eq!(record.len(), 40);
        assert_eq!(entity_key(1), [1, 0, 0, 0, 0, 0, 0, 0]);
    }

    #[test]
    fn long_names_are_truncated_and_terminated() {
        let record = entity(1, "0123456789012345678901234567890123456789");
        assert_eq!(record[8 + 31], 0);
        assert_eq!(record.len(), 40);
    }
}
