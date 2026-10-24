//! ZCL attribute frames: one frame carries a list of attributes.
//!
//! Portable view: parsing borrows the transport buffer, no allocation. Malformed
//! records are rejected here (truncated value, unknown data type, wrong command),
//! while unknown attribute ids are valid data and pass through.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrameError {
    /// Record does not fit into the payload.
    Truncated,
    /// ZCL data type is unknown, so the record length cannot be determined.
    UnknownType(u8),
    /// Not a Report Attributes (0x0A) or Read Attributes Response (0x01) frame.
    NotAttributeFrame(u8),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AttributeView<'a> {
    pub id: u16,
    pub zcl_type: u8,
    pub value: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AttributeFrameView<'a> {
    pub cluster: u16,
    pub endpoint: u8,
    pub command: u8,
    payload: &'a [u8],
}

impl<'a> AttributeFrameView<'a> {
    pub fn parse(
        cluster: u16,
        endpoint: u8,
        command: u8,
        payload: &'a [u8],
    ) -> Result<Self, FrameError> {
        if command != 0x0A && command != 0x01 {
            return Err(FrameError::NotAttributeFrame(command));
        }
        validate(command, payload)?;
        Ok(Self {
            cluster,
            endpoint,
            command,
            payload,
        })
    }

    pub fn attributes(&self) -> AttributeIter<'a> {
        AttributeIter {
            command: self.command,
            payload: self.payload,
            offset: 0,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct AttributeIter<'a> {
    command: u8,
    payload: &'a [u8],
    offset: usize,
}

impl<'a> Iterator for AttributeIter<'a> {
    type Item = AttributeView<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        while self.offset < self.payload.len() {
            let rest = &self.payload[self.offset..];
            if rest.len() < 2 {
                return None;
            }
            let id = u16::from_le_bytes([rest[0], rest[1]]);
            let mut used = 2;
            if self.command == 0x01 {
                let status = rest.get(2)?;
                used += 1;
                if *status != 0 {
                    self.offset += used;
                    continue;
                }
            }
            let zcl_type = *rest.get(used)?;
            used += 1;
            let (prefix, length) = record_parts(zcl_type, &rest[used..]).ok()?;
            let value_start = self.offset + used + prefix;
            let value = self.payload.get(value_start..value_start + length)?;
            self.offset = value_start + length;
            return Some(AttributeView {
                id,
                zcl_type,
                value,
            });
        }
        None
    }
}

fn validate(command: u8, payload: &[u8]) -> Result<(), FrameError> {
    let mut offset = 0;
    while offset < payload.len() {
        let rest = &payload[offset..];
        if rest.len() < 2 {
            return Err(FrameError::Truncated);
        }
        let mut used = 2;
        if command == 0x01 {
            let status = *rest.get(2).ok_or(FrameError::Truncated)?;
            used += 1;
            if status != 0 {
                offset += used;
                continue;
            }
        }
        let zcl_type = *rest.get(used).ok_or(FrameError::Truncated)?;
        used += 1;
        let (prefix, length) = record_parts(zcl_type, &rest[used..])?;
        let end = offset + used + prefix + length;
        if end > payload.len() {
            return Err(FrameError::Truncated);
        }
        offset = end;
    }
    Ok(())
}

/// Length of the value bytes; for variable types the length prefix is included
/// in `rest`, and the returned length is the value length only.
fn record_parts(zcl_type: u8, rest: &[u8]) -> Result<(usize, usize), FrameError> {
    let fixed = match zcl_type {
        0x00 | 0xFF => 0,
        0x08..=0x0F => usize::from(zcl_type - 0x07),
        0x10 | 0x18 | 0x20 | 0x28 | 0x30 => 1,
        0x19 | 0x21 | 0x29 | 0x31 | 0x38 | 0xE8 | 0xE9 => 2,
        0x22 | 0x2A => 3,
        0x23 | 0x2B | 0x39 | 0xE0 | 0xE1 | 0xE2 | 0xEA => 4,
        0x24 | 0x2C => 5,
        0x25 | 0x2D => 6,
        0x26 | 0x2E => 7,
        0x27 | 0x2F | 0x3A | 0xF0 => 8,
        0xF1 => 16,
        0x41 | 0x42 => {
            let len = usize::from(*rest.first().ok_or(FrameError::Truncated)?);
            return check(rest, 1, len).map(|len| (1, len));
        }
        0x43 | 0x44 => {
            let bytes = rest.get(..2).ok_or(FrameError::Truncated)?;
            let len = usize::from(u16::from_le_bytes([bytes[0], bytes[1]]));
            return check(rest, 2, len).map(|len| (2, len));
        }
        other => return Err(FrameError::UnknownType(other)),
    };
    Ok((0, fixed))
}

fn check(rest: &[u8], prefix: usize, value_len: usize) -> Result<usize, FrameError> {
    if rest.len() < prefix + value_len {
        return Err(FrameError::Truncated);
    }
    Ok(value_len)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn report(payload: &[u8]) -> AttributeFrameView<'_> {
        AttributeFrameView::parse(0x0001, 1, 0x0A, payload).unwrap()
    }

    #[test]
    fn multiple_attributes_in_one_frame() {
        // batteryPercentageRemaining (0x21, uint8=200), batteryVoltage (0x20, uint8=30)
        let frame = report(&[0x21, 0x00, 0x20, 200, 0x20, 0x00, 0x20, 30]);
        let attrs: Vec<AttributeView<'_>> = frame.attributes().collect();
        assert_eq!(attrs.len(), 2);
        assert_eq!(attrs[0].id, 0x0021);
        assert_eq!(attrs[0].zcl_type, 0x20);
        assert_eq!(attrs[0].value, &[200]);
        assert_eq!(attrs[1].id, 0x0020);
        assert_eq!(attrs[1].value, &[30]);
    }

    #[test]
    fn unknown_attribute_id_is_data_not_an_error() {
        let frame = report(&[0x55, 0x00, 0x20, 1]);
        let attrs: Vec<_> = frame.attributes().collect();
        assert_eq!(attrs.len(), 1);
        assert_eq!(attrs[0].id, 0x0055);
    }

    #[test]
    fn variable_length_string_is_parsed() {
        // char string "ab": type 0x42, len 2, bytes
        let frame = report(&[0x01, 0x00, 0x42, 2, b'a', b'b', 0x02, 0x00, 0x20, 7]);
        let attrs: Vec<_> = frame.attributes().collect();
        assert_eq!(attrs.len(), 2);
        assert_eq!(attrs[0].value, b"ab");
        assert_eq!(attrs[1].value, &[7]);
    }

    #[test]
    fn truncated_value_is_malformed() {
        // uint16 needs 2 bytes, only 1 present
        assert_eq!(
            AttributeFrameView::parse(0x0001, 1, 0x0A, &[0x21, 0x00, 0x21, 1]),
            Err(FrameError::Truncated)
        );
        // missing type byte
        assert_eq!(
            AttributeFrameView::parse(0x0001, 1, 0x0A, &[0x21, 0x00]),
            Err(FrameError::Truncated)
        );
    }

    #[test]
    fn unknown_type_and_wrong_command_are_rejected() {
        assert_eq!(
            AttributeFrameView::parse(0x0001, 1, 0x0A, &[0x21, 0x00, 0xEE, 0]),
            Err(FrameError::UnknownType(0xEE))
        );
        assert_eq!(
            AttributeFrameView::parse(0x0001, 1, 0x0B, &[]),
            Err(FrameError::NotAttributeFrame(0x0B))
        );
    }

    #[test]
    fn read_response_skips_failed_records() {
        // success (status 0) with uint8=5, failure (status 1), success uint8=9
        let frame =
            AttributeFrameView::parse(0x0001, 1, 0x01, &[0x01, 0x00, 0x00, 0x20, 5, 0x02, 0x00, 0x01, 0x03, 0x00, 0x00, 0x20, 9])
                .unwrap();
        let attrs: Vec<_> = frame.attributes().collect();
        assert_eq!(attrs.len(), 2);
        assert_eq!(attrs[0].id, 0x0001);
        assert_eq!(attrs[1].id, 0x0003);
    }

    #[test]
    fn empty_payload_is_a_valid_empty_frame() {
        let frame = report(&[]);
        assert_eq!(frame.attributes().count(), 0);
    }
}
