use zigbee_devices::{
    attribute_capability, classify_endpoint, decode_standard, update_is_discovered, ClusterRole, Confirmed, DeviceKind,
    EndpointDescriptor, StandardCapability,
};
use zigbee_devices::standard::{ONOFF, TEMPERATURE};
use z2m_rs::{properties, RawZcl, Update, Value};

fn endpoint(clusters: &[(u16, ClusterRole)]) -> EndpointDescriptor {
    let mut endpoint = EndpointDescriptor::new(1, 0x0104, 0x0000);
    for (cluster, role) in clusters {
        assert!(endpoint.push(*cluster, *role));
    }
    endpoint
}

#[test]
fn standard_decode_requires_server_cluster() {
    let relay = endpoint(&[(ONOFF, ClusterRole::Server)]);
    assert_eq!(classify_endpoint(&relay).kind, DeviceKind::Relay);
    let event = RawZcl { device: [1; 8], endpoint: 1, cluster: ONOFF, command: 0x0A, attribute: Some(0x0000), payload: &[1] };
    let decoded = decode_standard(&relay, &event).expect("relay decode");
    assert_eq!(decoded.updates().next().copied(), Some(Update::Property { property: properties::STATE, value: Value::Bool(true) }));
    let button = endpoint(&[(ONOFF, ClusterRole::Client)]);
    assert_eq!(classify_endpoint(&button).kind, DeviceKind::ButtonCandidate);
    assert!(decode_standard(&button, &event).is_none());
}

#[test]
fn standard_temperature_is_gated_by_discovered_cluster() {
    let sensor = endpoint(&[(TEMPERATURE, ClusterRole::Server)]);
    let event = RawZcl { device: [1; 8], endpoint: 1, cluster: TEMPERATURE, command: 0x0A, attribute: Some(0x0000), payload: &[0x2c, 0x01] };
    let decoded = decode_standard(&sensor, &event).expect("temperature decode");
    let update = decoded.updates().next().copied().unwrap();
    assert_eq!(update, Update::Property { property: properties::TEMPERATURE, value: Value::Float(3.0) });
    assert!(update_is_discovered(&sensor, &update));
    let relay = endpoint(&[(ONOFF, ClusterRole::Server)]);
    assert!(!update_is_discovered(&relay, &update));
    assert!(classify_endpoint(&sensor).features.contains(StandardCapability::Temperature));
}

#[test]
fn discovery_does_not_confirm_attributes() {
    let mut confirmed = Confirmed::new();
    assert!(!confirmed.is_confirmed(1, ONOFF, 0x0000));
    assert!(confirmed.confirm(1, ONOFF, 0x0000));
    assert!(confirmed.is_confirmed(1, ONOFF, 0x0000));
    assert!(confirmed.confirm(1, ONOFF, 0x0000));
    assert_eq!(confirmed.len(), 1);
}

#[test]
fn attribute_confirmation_requires_a_real_attribute() {
    let mut confirmed = Confirmed::new();
    assert_eq!(attribute_capability(ONOFF, 0x0000), Some(StandardCapability::Power));
    assert_eq!(attribute_capability(ONOFF, 0x0001), None);
    assert!(confirmed.confirm(1, ONOFF, 0x0000));
    assert!(confirmed.is_confirmed(1, ONOFF, 0x0000));
    assert!(!confirmed.is_confirmed(1, ONOFF, 0x0001));
}

#[test]
fn confirmed_attribute_upgrades_capabilities() {
    let relay = endpoint(&[(ONOFF, ClusterRole::Server)]);
    let mut confirmed = Confirmed::new();
    let empty = zigbee_devices::standard::capabilities_confirmed(&relay, &confirmed);
    assert_eq!(empty.property_count, 0);
    assert!(confirmed.confirm(1, ONOFF, 0x0000));
    let caps = zigbee_devices::standard::capabilities_confirmed(&relay, &confirmed);
    assert!(caps.properties[..usize::from(caps.property_count)].iter().any(|c| c.property == 1));
}
