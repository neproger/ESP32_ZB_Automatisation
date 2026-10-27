//! Standard ZCL cluster metadata and attribute confirmation.
//!
//! Cluster discovery proves that a cluster is advertised, not that an
//! attribute is implemented. Attributes are confirmed by a real read/report.

use ha_domain::canonical::{
    PropertyId, PROPERTY_BATTERY_PERCENT, PROPERTY_BATTERY_VOLTAGE, PROPERTY_BRIGHTNESS,
    PROPERTY_COLOR, PROPERTY_COLOR_TEMPERATURE, PROPERTY_HUMIDITY, PROPERTY_ILLUMINANCE,
    PROPERTY_OCCUPANCY, PROPERTY_POWER, PROPERTY_TEMPERATURE,
};

pub const ONOFF: u16 = 0x0006;
pub const LEVEL: u16 = 0x0008;
pub const COLOR: u16 = 0x0300;
pub const POWER_CONFIG: u16 = 0x0001;
pub const ILLUMINANCE: u16 = 0x0400;
pub const TEMPERATURE: u16 = 0x0402;
pub const HUMIDITY: u16 = 0x0405;
pub const OCCUPANCY: u16 = 0x0406;

pub const MAX_CONFIRMED_ATTRIBUTES: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Confirmed {
    entries: [Option<(u8, u16, u16)>; MAX_CONFIRMED_ATTRIBUTES],
    len: u8,
}

impl Default for Confirmed {
    fn default() -> Self {
        Self::new()
    }
}

impl Confirmed {
    pub const fn new() -> Self {
        Self {
            entries: [None; MAX_CONFIRMED_ATTRIBUTES],
            len: 0,
        }
    }

    pub fn confirm(&mut self, endpoint: u8, cluster: u16, attribute: u16) -> bool {
        if self.is_confirmed(endpoint, cluster, attribute) {
            return true;
        }
        let index = usize::from(self.len);
        if index == MAX_CONFIRMED_ATTRIBUTES {
            return false;
        }
        self.entries[index] = Some((endpoint, cluster, attribute));
        self.len += 1;
        true
    }

    pub fn is_confirmed(&self, endpoint: u8, cluster: u16, attribute: u16) -> bool {
        self.entries[..usize::from(self.len)]
            .iter()
            .flatten()
            .any(|entry| *entry == (endpoint, cluster, attribute))
    }

    pub fn len(&self) -> usize {
        usize::from(self.len)
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

/// Standard attribute(s) backing a canonical property. Color accepts either
/// XY attribute; all of them must be confirmed for that endpoint.
pub fn property_attributes(property: PropertyId) -> Option<(u16, &'static [u16])> {
    Some(match property {
        PROPERTY_POWER => (ONOFF, &[0x0000]),
        PROPERTY_BRIGHTNESS => (LEVEL, &[0x0000]),
        PROPERTY_COLOR_TEMPERATURE => (COLOR, &[0x0007]),
        PROPERTY_COLOR => (COLOR, &[0x0003, 0x0004]),
        PROPERTY_BATTERY_PERCENT => (POWER_CONFIG, &[0x0021]),
        PROPERTY_BATTERY_VOLTAGE => (POWER_CONFIG, &[0x0020]),
        PROPERTY_TEMPERATURE => (TEMPERATURE, &[0x0000]),
        PROPERTY_HUMIDITY => (HUMIDITY, &[0x0000]),
        PROPERTY_ILLUMINANCE => (ILLUMINANCE, &[0x0000]),
        PROPERTY_OCCUPANCY => (OCCUPANCY, &[0x0000]),
        _ => return None,
    })
}

/// Inverse of `property_attributes` for a reported attribute.
pub fn attribute_property(cluster: u16, attribute: u16) -> Option<PropertyId> {
    Some(match (cluster, attribute) {
        (ONOFF, 0x0000) => PROPERTY_POWER,
        (LEVEL, 0x0000) => PROPERTY_BRIGHTNESS,
        (COLOR, 0x0007) => PROPERTY_COLOR_TEMPERATURE,
        (COLOR, 0x0003) | (COLOR, 0x0004) => PROPERTY_COLOR,
        (POWER_CONFIG, 0x0021) => PROPERTY_BATTERY_PERCENT,
        (POWER_CONFIG, 0x0020) => PROPERTY_BATTERY_VOLTAGE,
        (TEMPERATURE, 0x0000) => PROPERTY_TEMPERATURE,
        (HUMIDITY, 0x0000) => PROPERTY_HUMIDITY,
        (ILLUMINANCE, 0x0000) => PROPERTY_ILLUMINANCE,
        (OCCUPANCY, 0x0000) => PROPERTY_OCCUPANCY,
        _ => return None,
    })
}
