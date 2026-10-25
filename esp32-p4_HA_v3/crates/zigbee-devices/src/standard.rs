//! Standard ZCL classification from discovered endpoint clusters.
//!
//! This is deliberately provisional: a cluster in a Simple Descriptor proves
//! that the endpoint advertises it, not that every attribute or command is
//! implemented. Attributes are confirmed later by Read/Discover Attributes or a
//! real report.

use ha_domain::canonical::{
    PROPERTY_BATTERY_PERCENT, PROPERTY_BATTERY_VOLTAGE, PROPERTY_BRIGHTNESS, PROPERTY_COLOR,
    PROPERTY_COLOR_TEMPERATURE, PROPERTY_HUMIDITY, PROPERTY_ILLUMINANCE, PROPERTY_OCCUPANCY,
    PROPERTY_POWER, PropertyId,
};

use crate::descriptor::{ClusterRole, DeviceDescriptor, EndpointDescriptor};

pub const ONOFF: u16 = 0x0006;
pub const LEVEL: u16 = 0x0008;
pub const COLOR: u16 = 0x0300;
pub const POWER_CONFIG: u16 = 0x0001;
pub const ILLUMINANCE: u16 = 0x0400;
pub const TEMPERATURE: u16 = 0x0402;
pub const HUMIDITY: u16 = 0x0405;
pub const OCCUPANCY: u16 = 0x0406;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StandardCapability {
    /// OnOff state. Commands: on/off/toggle.
    Power,
    /// Level state, normally paired with Power.
    Brightness,
    ColorTemperature,
    Color,
    Temperature,
    Humidity,
    Illuminance,
    Occupancy,
    BatteryPercent,
    BatteryVoltage,
}

impl StandardCapability {
    pub const fn property(self) -> PropertyId {
        match self {
            Self::Power => PROPERTY_POWER,
            Self::Brightness => PROPERTY_BRIGHTNESS,
            Self::ColorTemperature => PROPERTY_COLOR_TEMPERATURE,
            Self::Color => PROPERTY_COLOR,
            Self::Temperature => ha_domain::canonical::PROPERTY_TEMPERATURE,
            Self::Humidity => PROPERTY_HUMIDITY,
            Self::Illuminance => PROPERTY_ILLUMINANCE,
            Self::Occupancy => PROPERTY_OCCUPANCY,
            Self::BatteryPercent => PROPERTY_BATTERY_PERCENT,
            Self::BatteryVoltage => PROPERTY_BATTERY_VOLTAGE,
        }
    }
}

pub const MAX_FEATURES: usize = 10;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EndpointFeatures {
    pub endpoint: u8,
    features: [Option<StandardCapability>; MAX_FEATURES],
    feature_count: u8,
    /// Client-side OnOff. A device may send OnOff commands to us; this is not
    /// proof of single/double/hold, which is not standard ZCL.
    pub button_candidate: bool,
}

impl EndpointFeatures {
    pub const fn new(endpoint: u8) -> Self {
        Self {
            endpoint,
            features: [None; MAX_FEATURES],
            feature_count: 0,
            button_candidate: false,
        }
    }

    fn push(&mut self, feature: StandardCapability) {
        if self.features[..usize::from(self.feature_count)].contains(&Some(feature)) {
            return;
        }
        let index = usize::from(self.feature_count);
        if index == MAX_FEATURES {
            return;
        }
        self.features[index] = Some(feature);
        self.feature_count += 1;
    }

    pub fn contains(&self, feature: StandardCapability) -> bool {
        self.features[..usize::from(self.feature_count)].contains(&Some(feature))
    }

    pub fn features(&self) -> impl Iterator<Item = StandardCapability> + '_ {
        self.features[..usize::from(self.feature_count)]
            .iter()
            .flatten()
            .copied()
    }

    pub fn len(&self) -> usize {
        usize::from(self.feature_count)
    }

    pub fn is_empty(&self) -> bool {
        self.feature_count == 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceKind {
    Relay,
    Dimmer,
    ColorLight,
    Sensor,
    ButtonCandidate,
    Unknown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EndpointClass {
    pub kind: DeviceKind,
    pub features: EndpointFeatures,
}

/// Classify one endpoint using cluster ids and server/client direction.
/// No attribute is claimed as confirmed here.
pub fn classify_endpoint(endpoint: &EndpointDescriptor) -> EndpointClass {
    let mut features = EndpointFeatures::new(endpoint.endpoint);

    if endpoint.has_server(ONOFF) {
        features.push(StandardCapability::Power);
    }
    if endpoint.has_server(LEVEL) {
        features.push(StandardCapability::Brightness);
    }
    if endpoint.has_server(COLOR) {
        features.push(StandardCapability::ColorTemperature);
        features.push(StandardCapability::Color);
    }
    if endpoint.has_server(POWER_CONFIG) {
        features.push(StandardCapability::BatteryPercent);
        features.push(StandardCapability::BatteryVoltage);
    }
    if endpoint.has_server(TEMPERATURE) {
        features.push(StandardCapability::Temperature);
    }
    if endpoint.has_server(HUMIDITY) {
        features.push(StandardCapability::Humidity);
    }
    if endpoint.has_server(ILLUMINANCE) {
        features.push(StandardCapability::Illuminance);
    }
    if endpoint.has_server(OCCUPANCY) {
        features.push(StandardCapability::Occupancy);
    }
    features.button_candidate = endpoint.has_client(ONOFF);

    let kind = if features.button_candidate && !features.contains(StandardCapability::Power) {
        DeviceKind::ButtonCandidate
    } else if features.contains(StandardCapability::Color) {
        DeviceKind::ColorLight
    } else if features.contains(StandardCapability::Brightness) {
        DeviceKind::Dimmer
    } else if features.contains(StandardCapability::Power) {
        DeviceKind::Relay
    } else if features.len() > 0 {
        DeviceKind::Sensor
    } else {
        DeviceKind::Unknown
    };

    EndpointClass { kind, features }
}

pub fn classify_device(device: &DeviceDescriptor) -> impl Iterator<Item = EndpointClass> + '_ {
    device.endpoints().map(classify_endpoint)
}

/// True when the endpoint advertises the cluster on the side that owns the
/// attributes and accepts commands from the coordinator.
pub fn has_server(endpoint: &EndpointDescriptor, cluster: u16) -> bool {
    endpoint.clusters().iter().any(|entry| {
        entry.cluster_id == cluster && entry.role == ClusterRole::Server
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::descriptor::EndpointDescriptor;

    fn endpoint(clusters: &[(u16, ClusterRole)]) -> EndpointDescriptor {
        let mut endpoint = EndpointDescriptor::new(1, 0x0104, 0x0000);
        for (cluster, role) in clusters {
            assert!(endpoint.push(*cluster, *role));
        }
        endpoint
    }

    #[test]
    fn relay_is_server_onoff_only() {
        let class = classify_endpoint(&endpoint(&[(ONOFF, ClusterRole::Server)]));
        assert_eq!(class.kind, DeviceKind::Relay);
        assert!(class.features.contains(StandardCapability::Power));
        assert!(!class.features.contains(StandardCapability::Brightness));
        assert!(!class.features.button_candidate);
    }

    #[test]
    fn dimmer_and_color_light_are_distinguished() {
        let dimmer = classify_endpoint(&endpoint(&[
            (ONOFF, ClusterRole::Server),
            (LEVEL, ClusterRole::Server),
        ]));
        assert_eq!(dimmer.kind, DeviceKind::Dimmer);
        let color = classify_endpoint(&endpoint(&[
            (ONOFF, ClusterRole::Server),
            (LEVEL, ClusterRole::Server),
            (COLOR, ClusterRole::Server),
        ]));
        assert_eq!(color.kind, DeviceKind::ColorLight);
        assert!(color.features.contains(StandardCapability::Color));
    }

    #[test]
    fn client_onoff_is_only_a_button_candidate() {
        let class = classify_endpoint(&endpoint(&[(ONOFF, ClusterRole::Client)]));
        assert_eq!(class.kind, DeviceKind::ButtonCandidate);
        assert!(class.features.button_candidate);
        assert!(!class.features.contains(StandardCapability::Power));
        assert_eq!(class.features.len(), 0);
    }

    #[test]
    fn sensors_use_server_measurement_clusters() {
        let class = classify_endpoint(&endpoint(&[
            (TEMPERATURE, ClusterRole::Server),
            (HUMIDITY, ClusterRole::Server),
            (POWER_CONFIG, ClusterRole::Server),
        ]));
        assert_eq!(class.kind, DeviceKind::Sensor);
        assert!(class.features.contains(StandardCapability::Temperature));
        assert!(class.features.contains(StandardCapability::Humidity));
        assert!(class.features.contains(StandardCapability::BatteryPercent));
    }

    #[test]
    fn client_measurement_cluster_is_not_a_sensor_capability() {
        let class = classify_endpoint(&endpoint(&[(TEMPERATURE, ClusterRole::Client)]));
        assert_eq!(class.kind, DeviceKind::Unknown);
        assert!(!class.features.contains(StandardCapability::Temperature));
    }
}
/// Identity-only profile for devices with no ZHC profile. Standard capabilities
/// come from the discovered cluster classification and ownership, not here.
pub static STANDARD_PROFILE: z2m_rs::Profile = z2m_rs::Profile {
    id: "standard.zcl",
    vendor: "ZCL",
    fingerprints: &[],
    exposes: &[],
    from: &[],
    to: &[],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: z2m_rs::Support::PARTIAL,
};
