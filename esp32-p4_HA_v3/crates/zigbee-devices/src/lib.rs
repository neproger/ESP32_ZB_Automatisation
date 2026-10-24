//! HA Zigbee Layer: the only place that knows both `z2m-rs` and HA.
//!
//! It binds raw ZCL events to a `z2m-rs` profile and exposes normalized device
//! updates. HA types (Domain facts, canonical records) will be produced here, in
//! one adapter — `z2m-rs` stays free of HA and `zigbee-esp` stays free of vendor
//! rules.

use z2m_rs::{decode, Action, RawZcl, Update as Z2mUpdate};

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

/// What a device model reports; HA mapping happens above this crate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceUpdate {
    pub device: [u8; 8],
    pub property: Property,
}

pub trait DeviceAdapter {
    fn decode(&self, event: &RawZclEvent<'_>) -> Option<DeviceUpdate>;
}

/// Minimal TS0041 adapter: delegates the wire semantics to the Z2M port.
pub struct TuyaButton;

impl DeviceAdapter for TuyaButton {
    fn decode(&self, event: &RawZclEvent<'_>) -> Option<DeviceUpdate> {
        let raw = RawZcl {
            device: event.eui64.to_be_bytes(),
            endpoint: event.endpoint,
            cluster: event.cluster,
            command: event.command,
            attribute: None,
            payload: event.payload,
        };
        let decoded = decode(&z2m_rs::TUYA_TS0041, &raw)?;
        let action = decoded.updates().find_map(|update| match update {
            Z2mUpdate::Action(action) => Some(*action),
            Z2mUpdate::Property { .. } => None,
        })?;
        Some(DeviceUpdate {
            device: raw.device,
            property: Property::Button(match action {
                Action::Single => ButtonAction::Single,
                Action::Double => ButtonAction::Double,
                Action::Hold => ButtonAction::Hold,
            }),
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
    }
}
