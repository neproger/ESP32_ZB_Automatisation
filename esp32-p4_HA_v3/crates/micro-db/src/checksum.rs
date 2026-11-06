//! V2-compatible checksums and persisted-region identifiers.

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
