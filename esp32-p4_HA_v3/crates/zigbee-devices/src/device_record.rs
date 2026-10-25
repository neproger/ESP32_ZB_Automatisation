//! Canonical DeviceRecord: one binary layout for Domain, MicroDB and WebSocket.
//!
//! The layout is explicit (little-endian, no padding, no NULs), versioned by
//! `schema`, and has a fixed header plus variable strings and an opaque
//! endpoint/cluster signature. `#[repr(C)]` is deliberately not used: the wire
//! format must not depend on the compiler.
//!
//! Header (24 bytes):
//! ```text
//! 0  u8   schema = 1
//! 1  u8   interview status
//! 2  u16  header_len
//! 4  u64  ieee (durable identity)
//! 12 u16  short (cache only, 0 = unknown)
//! 14 u16  definition_version   (configure logic of this profile)
//! 16 u16  configured_version
//! 18 u8   definition_id_len
//! 19 u8   manufacturer_len
//! 20 u8   model_len
//! 21 u8   reserved = 0
//! 22 u16  signature_len
//! 24 ...  definition_id | manufacturer | model | signature
//! ```

pub const DEVICE_SCHEMA: u8 = 1;
pub const HEADER_LEN: usize = 24;
pub const MAX_RECORD: usize = 512;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InterviewStatus {
    None = 0,
    Partial = 1,
    Complete = 2,
    Failed = 3,
    /// Deferred-leave tombstone: the device was removed from HA but its Zigbee
    /// leave has not been confirmed yet.
    Removed = 4,
}

impl InterviewStatus {
    fn from_u8(value: u8) -> Option<Self> {
        Some(match value {
            0 => Self::None,
            1 => Self::Partial,
            2 => Self::Complete,
            3 => Self::Failed,
            4 => Self::Removed,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecordError {
    TooLong,
    Truncated,
    UnsupportedSchema(u8),
    InvalidHeader,
    InvalidUtf8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceRecord<'a> {
    pub ieee: u64,
    pub short: u16,
    pub interview: InterviewStatus,
    pub definition_id: &'a str,
    pub manufacturer: &'a str,
    pub model: &'a str,
    pub definition_version: u16,
    pub configured_version: u16,
    /// Extensible endpoint/cluster signature; encoding is a separate contract.
    pub signature: &'a [u8],
}

impl<'a> DeviceRecord<'a> {
    pub fn encode(&self, out: &mut [u8]) -> Result<usize, RecordError> {
        let lengths = [
            self.definition_id.len(),
            self.manufacturer.len(),
            self.model.len(),
        ];
        if lengths.iter().any(|len| *len > u8::MAX as usize)
            || self.signature.len() > u16::MAX as usize
        {
            return Err(RecordError::TooLong);
        }
        let total = HEADER_LEN + lengths.iter().sum::<usize>() + self.signature.len();
        if total > MAX_RECORD || out.len() < total {
            return Err(RecordError::TooLong);
        }
        out[..total].fill(0);
        out[0] = DEVICE_SCHEMA;
        out[1] = self.interview as u8;
        put_u16(out, 2, HEADER_LEN as u16);
        put_u64(out, 4, self.ieee);
        put_u16(out, 12, self.short);
        put_u16(out, 14, self.definition_version);
        put_u16(out, 16, self.configured_version);
        out[18] = lengths[0] as u8;
        out[19] = lengths[1] as u8;
        out[20] = lengths[2] as u8;
        out[21] = 0;
        put_u16(out, 22, self.signature.len() as u16);

        let mut offset = HEADER_LEN;
        for text in [self.definition_id, self.manufacturer, self.model] {
            out[offset..offset + text.len()].copy_from_slice(text.as_bytes());
            offset += text.len();
        }
        out[offset..offset + self.signature.len()].copy_from_slice(self.signature);
        Ok(total)
    }

    pub fn parse(bytes: &'a [u8]) -> Result<Self, RecordError> {
        if bytes.len() < HEADER_LEN {
            return Err(RecordError::Truncated);
        }
        if bytes[0] != DEVICE_SCHEMA {
            return Err(RecordError::UnsupportedSchema(bytes[0]));
        }
        if get_u16(bytes, 2) as usize != HEADER_LEN || bytes[21] != 0 {
            return Err(RecordError::InvalidHeader);
        }
        let interview = InterviewStatus::from_u8(bytes[1]).ok_or(RecordError::InvalidHeader)?;
        let lengths = [bytes[18] as usize, bytes[19] as usize, bytes[20] as usize];
        let signature_len = get_u16(bytes, 22) as usize;
        let total = HEADER_LEN + lengths.iter().sum::<usize>() + signature_len;
        if total != bytes.len() || total > MAX_RECORD {
            return Err(RecordError::Truncated);
        }
        let mut offset = HEADER_LEN;
        let definitions = [
            core::str::from_utf8(&bytes[offset..offset + lengths[0]])
                .map_err(|_| RecordError::InvalidUtf8)?,
            {
                offset += lengths[0];
                core::str::from_utf8(&bytes[offset..offset + lengths[1]])
                    .map_err(|_| RecordError::InvalidUtf8)?
            },
            {
                offset += lengths[1];
                core::str::from_utf8(&bytes[offset..offset + lengths[2]])
                    .map_err(|_| RecordError::InvalidUtf8)?
            },
        ];
        offset += lengths[2];
        Ok(Self {
            ieee: get_u64(bytes, 4),
            short: get_u16(bytes, 12),
            interview,
            definition_id: definitions[0],
            manufacturer: definitions[1],
            model: definitions[2],
            definition_version: get_u16(bytes, 14),
            configured_version: get_u16(bytes, 16),
            signature: &bytes[offset..offset + signature_len],
        })
    }
}

fn put_u16(buf: &mut [u8], offset: usize, value: u16) {
    buf[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(buf: &mut [u8], offset: usize, value: u64) {
    buf[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn get_u16(buf: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([buf[offset], buf[offset + 1]])
}

fn get_u64(buf: &[u8], offset: usize) -> u64 {
    let mut bytes = [0u8; 8];
    bytes.copy_from_slice(&buf[offset..offset + 8]);
    u64::from_le_bytes(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record<'a>(signature: &'a [u8]) -> DeviceRecord<'a> {
        DeviceRecord {
            ieee: 0xA4C1_383B_F9D6_2A03,
            short: 0xD07B,
            interview: InterviewStatus::Complete,
            definition_id: "tuya.ts0041",
            manufacturer: "_TZ3000_b4awzgct",
            model: "TS0041",
            definition_version: 1,
            configured_version: 1,
            signature,
        }
    }

    #[test]
    fn roundtrip_with_variable_signature() {
        let signature = [1u8, 2, 0x06, 0x00, 0x06, 0x00, 0x01];
        let mut buf = [0u8; MAX_RECORD];
        let len = record(&signature).encode(&mut buf).unwrap();
        assert_eq!(len, HEADER_LEN + 11 + 16 + 6 + signature.len());
        let decoded = DeviceRecord::parse(&buf[..len]).unwrap();
        assert_eq!(decoded, record(&signature));
    }

    #[test]
    fn header_is_explicit_little_endian_without_padding() {
        let mut buf = [0u8; MAX_RECORD];
        let len = record(&[]).encode(&mut buf).unwrap();
        assert_eq!(buf[0], DEVICE_SCHEMA);
        assert_eq!(buf[1], InterviewStatus::Complete as u8);
        assert_eq!(&buf[2..4], &(HEADER_LEN as u16).to_le_bytes());
        assert_eq!(&buf[4..12], &0xA4C1_383B_F9D6_2A03u64.to_le_bytes());
        assert_eq!(&buf[12..14], &0xD07Bu16.to_le_bytes());
        assert_eq!(buf[21], 0);
        assert_eq!(len, HEADER_LEN + 11 + 16 + 6);
    }

    #[test]
    fn truncation_and_bad_schema_are_rejected() {
        let mut buf = [0u8; MAX_RECORD];
        let len = record(&[9]).encode(&mut buf).unwrap();
        assert_eq!(
            DeviceRecord::parse(&buf[..len - 1]),
            Err(RecordError::Truncated)
        );
        assert_eq!(
            DeviceRecord::parse(&buf[..HEADER_LEN - 1]),
            Err(RecordError::Truncated)
        );
        buf[0] = 9;
        assert_eq!(
            DeviceRecord::parse(&buf[..len]),
            Err(RecordError::UnsupportedSchema(9))
        );
    }

    #[test]
    fn invalid_utf8_is_rejected() {
        let mut buf = [0u8; MAX_RECORD];
        let len = record(&[]).encode(&mut buf).unwrap();
        buf[HEADER_LEN] = 0xFF;
        assert_eq!(
            DeviceRecord::parse(&buf[..len]),
            Err(RecordError::InvalidUtf8)
        );
    }

    #[test]
    fn length_limits_are_explicit() {
        let long = "x".repeat(u8::MAX as usize + 1);
        let mut rec = record(&[]);
        rec.definition_id = &long;
        let mut buf = [0u8; MAX_RECORD * 2];
        assert_eq!(rec.encode(&mut buf), Err(RecordError::TooLong));
    }
}
