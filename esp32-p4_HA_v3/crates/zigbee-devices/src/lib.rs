//! Device layer: raw ZCL events -> normalized properties and actions.
//!
//! This is the seam for the future Zigbee2MQTT port (`fromZigbee`, fingerprints,
//! `exposes`). It knows nothing about the Espressif SDK, Domain, storage, UI or
//! Tuya-specific SDK calls: only the wire shape of the event and the model that
//! produced it.
//!
//! Stage decision: the minimal TS0041 adapter lives here, not in the Zigbee
//! adapter. V2 C converters are not ported.

/// Borrowed ZCL event, valid only for the duration of decoding.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RawZclEvent<'a> {
    pub eui64: u64,
    pub endpoint: u8,
    pub cluster: u16,
    pub command: u8,
    pub payload: &'a [u8],
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ButtonAction {
    Single = 0,
    Double = 1,
    Hold = 2,
}

impl ButtonAction {
    pub const fn code(self) -> u32 {
        self as u32
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Property {
    Button(ButtonAction),
}

/// What a device model reports; Domain decides how to store it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceUpdate {
    pub device: [u8; 8],
    pub property: Property,
}

pub trait DeviceAdapter {
    fn decode(&self, event: &RawZclEvent<'_>) -> Option<DeviceUpdate>;
}

/// Minimal TS0041 adapter: Tuya sends the press as OnOff command 0xFD.
pub struct TuyaButton;

const CLUSTER_ON_OFF: u16 = 0x0006;
const CMD_TUYA_BUTTON: u8 = 0xFD;

impl DeviceAdapter for TuyaButton {
    fn decode(&self, event: &RawZclEvent<'_>) -> Option<DeviceUpdate> {
        if event.cluster != CLUSTER_ON_OFF || event.command != CMD_TUYA_BUTTON {
            return None;
        }
        let action = match event.payload {
            [0x00] => ButtonAction::Single,
            [0x01] => ButtonAction::Double,
            [0x02] => ButtonAction::Hold,
            _ => return None,
        };
        Some(DeviceUpdate {
            device: event.eui64.to_be_bytes(),
            property: Property::Button(action),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const EUI64: u64 = 0xa4c1_383b_f9d6_2a03;

    fn event<'a>(cluster: u16, command: u8, payload: &'a [u8]) -> RawZclEvent<'a> {
        RawZclEvent {
            eui64: EUI64,
            endpoint: 1,
            cluster,
            command,
            payload,
        }
    }

    #[test]
    fn tuya_payloads_map_to_actions() {
        let adapter = TuyaButton;
        for (payload, action) in [
            (0x00u8, ButtonAction::Single),
            (0x01, ButtonAction::Double),
            (0x02, ButtonAction::Hold),
        ] {
            let update = adapter.decode(&event(0x0006, 0xFD, &[payload])).unwrap();
            assert_eq!(update.device, EUI64.to_be_bytes());
            assert_eq!(update.property, Property::Button(action));
        }
    }

    #[test]
    fn foreign_events_are_not_claimed() {
        let adapter = TuyaButton;
        assert!(adapter.decode(&event(0x0006, 0x01, &[0x00])).is_none());
        assert!(adapter.decode(&event(0x0001, 0xFD, &[0x00])).is_none());
        assert!(adapter.decode(&event(0x0006, 0xFD, &[])).is_none());
        assert!(adapter.decode(&event(0x0006, 0xFD, &[0x03])).is_none());
        assert!(adapter.decode(&event(0x0006, 0xFD, &[0x00, 0x01])).is_none());
    }
}
