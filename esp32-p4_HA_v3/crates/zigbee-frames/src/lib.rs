//! Fixed frame for the C -> Rust boundary and ZCL attribute frame access.
//!
//! One bounded copy happens in the C callback into a slot; from then on the
//! frame is parsed in place and `z2m-rs` receives borrowed attribute views.
//! The pool itself is owned by the caller (`ha-app`), so this crate only stores
//! one frame and never allocates.

use z2m_rs::{AttributeFrameView, FrameError};

pub const FRAME_CAPACITY: usize = 160;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameTooLarge;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ZclFrame<const CAP: usize> {
    pub cluster: u16,
    pub endpoint: u8,
    pub command: u8,
    len: usize,
    bytes: [u8; CAP],
}

impl<const CAP: usize> Default for ZclFrame<CAP> {
    fn default() -> Self {
        Self {
            cluster: 0,
            endpoint: 0,
            command: 0,
            len: 0,
            bytes: [0; CAP],
        }
    }
}

impl<const CAP: usize> ZclFrame<CAP> {
    /// Copies one frame into the slot; the only copy on this path.
    pub fn set(
        &mut self,
        cluster: u16,
        endpoint: u8,
        command: u8,
        data: &[u8],
    ) -> Result<(), FrameTooLarge> {
        if data.len() > CAP {
            return Err(FrameTooLarge);
        }
        self.cluster = cluster;
        self.endpoint = endpoint;
        self.command = command;
        self.len = data.len();
        self.bytes[..data.len()].copy_from_slice(data);
        Ok(())
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }

    pub fn parse(&self) -> Result<AttributeFrameView<'_>, FrameError> {
        AttributeFrameView::parse(self.cluster, self.endpoint, self.command, self.bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use z2m_rs::{decode_frame, properties, Decoded, Update, Value, SNZB_01P};

    const BATTERY_REPORT: [u8; 8] = [0x21, 0x00, 0x20, 200, 0x20, 0x00, 0x20, 30];

    #[test]
    fn frame_roundtrip_and_capacity() {
        let mut frame = ZclFrame::<FRAME_CAPACITY>::default();
        assert!(frame.bytes().is_empty());
        frame.set(0x0001, 1, 0x0A, &BATTERY_REPORT).unwrap();
        assert_eq!(frame.bytes(), BATTERY_REPORT);
        assert!(frame
            .set(0x0001, 1, 0x0A, &[0; FRAME_CAPACITY + 1])
            .is_err());
    }

    #[test]
    fn multi_attribute_frame_reaches_z2m_rs() {
        let mut frame = ZclFrame::<FRAME_CAPACITY>::default();
        frame.set(0x0001, 1, 0x0A, &BATTERY_REPORT).unwrap();
        let view = frame.parse().unwrap();
        let mut decoded = Decoded::default();
        assert_eq!(decode_frame(&SNZB_01P, &view, &mut decoded), 2);
        assert_eq!(
            decoded.updates().copied().collect::<Vec<_>>(),
            vec![
                Update::Property {
                    property: properties::BATTERY,
                    value: Value::Float(100.0)
                },
                Update::Property {
                    property: properties::VOLTAGE,
                    value: Value::Float(3000.0)
                }
            ]
        );
    }

    #[test]
    fn malformed_and_unknown_frames_are_not_confused() {
        let mut frame = ZclFrame::<FRAME_CAPACITY>::default();
        frame.set(0x0001, 1, 0x0A, &[0x21, 0x00, 0x21, 1]).unwrap();
        assert_eq!(frame.parse(), Err(FrameError::Truncated));

        frame.set(0x0001, 1, 0x0A, &[0x55, 0x00, 0x20, 1]).unwrap();
        let view = frame.parse().unwrap();
        let mut decoded = Decoded::default();
        assert_eq!(decode_frame(&SNZB_01P, &view, &mut decoded), 0);
    }
}
