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
    /// Server -> browser: payload = `[entity_type u8][key]`.
    pub const REMOVE: u8 = 0x03;
    pub const EVENT: u8 = 0xE0;
    pub const COMMAND: u8 = 0x20;
}

/// Entity frame types; values are the V2 canonical ABI and must not change.
pub mod entity {
    pub const ENTITY: u8 = 14;
    pub const PROPERTY_STATE: u8 = 15;
    pub const CAPABILITIES: u8 = 16;
    pub const LOGICAL_DEVICE: u8 = 17;
}

/// Browser -> device command ids implemented by the firmware.
pub mod command {
    pub const SNAPSHOT: u8 = 1;
    pub const SNAPSHOT_STATE: u8 = 2;
    pub const DISCOVERY: u8 = 8;
    pub const ENTITY_RENAME: u8 = 18;
    pub const PROPERTY_COMMAND: u8 = 19;
    pub const DEVICE_RENAME: u8 = 20;
    pub const LOGICAL_DEVICE_REMOVE: u8 = 21;
}

pub const HEADER_LEN: usize = 8;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PropertyCommand {
    pub entity: u64,
    pub property: u32,
    pub action: u8,
    pub value: ha_domain::canonical::CommandValue,
}

/// `ha_property_command_t` (32 bytes) as sent by the browser:
/// entity u64@0, property u32@8, action u8@12, value kind u32@16,
/// scalar kind u32@20, value/xy @24.
pub fn parse_property_command(payload: &[u8]) -> Option<PropertyCommand> {
    use ha_domain::canonical::CommandValue;
    use ha_domain::Value;

    if payload.len() < 32 {
        return None;
    }
    let entity = u64::from_le_bytes(payload[0..8].try_into().ok()?);
    let property = u32::from_le_bytes(payload[8..12].try_into().ok()?);
    let action = payload[12];
    let kind = u32::from_le_bytes(payload[16..20].try_into().ok()?);
    let value = match kind {
        1 => {
            let scalar = u32::from_le_bytes(payload[20..24].try_into().ok()?);
            let bytes: [u8; 4] = payload[24..28].try_into().ok()?;
            let value = match scalar {
                2 => Value::i32(i32::from_le_bytes(bytes)),
                3 => Value::u32(u32::from_le_bytes(bytes)),
                4 => Value::f32(f32::from_le_bytes(bytes)),
                5 => Value::enum_(u32::from_le_bytes(bytes)),
                _ => return None,
            };
            CommandValue::Scalar(value)
        }
        2 => CommandValue::Xy {
            x: f32::from_le_bytes(payload[20..24].try_into().ok()?),
            y: f32::from_le_bytes(payload[24..28].try_into().ok()?),
        },
        0 => CommandValue::None,
        _ => return None,
    };
    Some(PropertyCommand {
        entity,
        property,
        action,
        value,
    })
}

/// `ha_entity_rename_t` / `ha_device_rename_t`: id u64@0, name[32]@8.
pub fn parse_rename(payload: &[u8]) -> Option<(u64, String)> {
    if payload.len() < 40 {
        return None;
    }
    let id = u64::from_le_bytes(payload[0..8].try_into().ok()?);
    let end = payload[8..40]
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(32);
    let name = String::from_utf8_lossy(&payload[8..8 + end]).into_owned();
    Some((id, name))
}

/// `ha_device_id_t` (8 bytes) used by LOGICAL_DEVICE_REMOVE.
pub fn parse_device_id(payload: &[u8]) -> Option<u64> {
    if payload.len() < 8 {
        return None;
    }
    Some(u64::from_le_bytes(payload[0..8].try_into().ok()?))
}

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

    fn command_bytes(entity: u64, property: u32, action: u8, kind: u32) -> [u8; 32] {
        let mut out = [0u8; 32];
        out[0..8].copy_from_slice(&entity.to_le_bytes());
        out[8..12].copy_from_slice(&property.to_le_bytes());
        out[12] = action;
        out[16..20].copy_from_slice(&kind.to_le_bytes());
        out
    }

    #[test]
    fn property_command_reads_power_value_and_color() {
        use ha_domain::canonical::CommandValue;
        use ha_domain::Value;

        let entity = 0x1122_3344_5566_7788;
        let power = parse_property_command(&command_bytes(entity, 1, 1, 0)).unwrap();
        assert_eq!(power.value, CommandValue::None);
        assert_eq!(power.action, 1);

        let mut brightness = command_bytes(entity, 2, 4, 1);
        brightness[20..24].copy_from_slice(&4u32.to_le_bytes());
        brightness[24..28].copy_from_slice(&50.0f32.to_le_bytes());
        assert_eq!(
            parse_property_command(&brightness).unwrap().value,
            CommandValue::Scalar(Value::f32(50.0))
        );

        let mut color = command_bytes(entity, 22, 4, 2);
        color[20..24].copy_from_slice(&0.25f32.to_le_bytes());
        color[24..28].copy_from_slice(&0.5f32.to_le_bytes());
        assert_eq!(
            parse_property_command(&color).unwrap().value,
            CommandValue::Xy { x: 0.25, y: 0.5 }
        );

        assert!(parse_property_command(&command_bytes(entity, 1, 1, 9)).is_none());
        assert!(parse_property_command(&brightness[..31]).is_none());
    }

    #[test]
    fn rename_and_device_id_follow_the_wire_layout() {
        let mut payload = [0u8; 40];
        payload[0..8].copy_from_slice(&0x1122_3344_5566_7788u64.to_le_bytes());
        payload[8..12].copy_from_slice(b"Lamp");
        assert_eq!(
            parse_rename(&payload),
            Some((0x1122_3344_5566_7788, "Lamp".to_string()))
        );
        assert_eq!(parse_device_id(&payload[..8]), Some(0x1122_3344_5566_7788));
        assert!(parse_rename(&payload[..39]).is_none());
        assert!(parse_device_id(&payload[..7]).is_none());
    }
}
