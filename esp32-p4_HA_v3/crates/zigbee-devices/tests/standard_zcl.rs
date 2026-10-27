use ha_domain::canonical::{ACTION_ON, ACTION_TOGGLE, PROPERTY_POWER, PROPERTY_TEMPERATURE};
use z2m_rs::{properties, RawZcl, Update, Value};
use zigbee_devices::standard::{
    attribute_capability, capabilities_confirmed, capability_allows, decode_standard,
    update_is_discovered, ONOFF, TEMPERATURE,
};
use zigbee_devices::{ClusterRole, Confirmed, EndpointDescriptor};

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
    let button = endpoint(&[(ONOFF, ClusterRole::Client)]);
    assert!(relay.has_server(ONOFF));
    assert!(!button.has_server(ONOFF));
    let event = RawZcl {
        device: [1; 8],
        endpoint: 1,
        cluster: ONOFF,
        command: 0x0A,
        attribute: Some(0x0000),
        payload: &[1],
    };
    let decoded = decode_standard(&relay, &event).expect("relay decode");
    assert_eq!(
        decoded.updates().next().copied(),
        Some(Update::Property {
            property: properties::STATE,
            value: Value::Bool(true)
        })
    );
    assert!(decode_standard(&button, &event).is_none());
}

#[test]
fn standard_temperature_is_gated_by_discovered_cluster() {
    let sensor = endpoint(&[(TEMPERATURE, ClusterRole::Server)]);
    let event = RawZcl {
        device: [1; 8],
        endpoint: 1,
        cluster: TEMPERATURE,
        command: 0x0A,
        attribute: Some(0x0000),
        payload: &[0x2c, 0x01],
    };
    let decoded = decode_standard(&sensor, &event).expect("temperature decode");
    let update = decoded.updates().next().copied().unwrap();
    assert_eq!(
        update,
        Update::Property {
            property: properties::TEMPERATURE,
            value: Value::Float(3.0)
        }
    );
    assert!(update_is_discovered(&sensor, &update));
    let relay = endpoint(&[(ONOFF, ClusterRole::Server)]);
    assert!(!update_is_discovered(&relay, &update));
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
    assert_eq!(attribute_capability(ONOFF, 0x0000), Some(PROPERTY_POWER));
    assert_eq!(attribute_capability(ONOFF, 0x0001), None);
    assert_eq!(
        attribute_capability(TEMPERATURE, 0x0000),
        Some(PROPERTY_TEMPERATURE)
    );
    assert!(confirmed.confirm(1, ONOFF, 0x0000));
    assert!(confirmed.is_confirmed(1, ONOFF, 0x0000));
    assert!(!confirmed.is_confirmed(1, ONOFF, 0x0001));
}

#[test]
fn standard_command_requires_confirmed_attribute() {
    let relay = endpoint(&[(ONOFF, ClusterRole::Server)]);
    let mut confirmed = Confirmed::new();
    assert!(!capability_allows(
        &relay,
        &confirmed,
        PROPERTY_POWER,
        ACTION_ON,
    ));
    assert!(!capability_allows(
        &relay,
        &confirmed,
        PROPERTY_POWER,
        ACTION_TOGGLE,
    ));
    assert!(confirmed.confirm(1, ONOFF, 0x0000));
    assert!(capability_allows(
        &relay,
        &confirmed,
        PROPERTY_POWER,
        ACTION_ON,
    ));
    assert!(capability_allows(
        &relay,
        &confirmed,
        PROPERTY_POWER,
        ACTION_TOGGLE,
    ));
}

#[test]
fn confirmed_attribute_upgrades_capabilities() {
    let relay = endpoint(&[(ONOFF, ClusterRole::Server)]);
    let mut confirmed = Confirmed::new();
    let empty = capabilities_confirmed(&relay, &confirmed);
    assert_eq!(empty.property_count, 0);
    assert!(confirmed.confirm(1, ONOFF, 0x0000));
    let caps = capabilities_confirmed(&relay, &confirmed);
    let power = caps.properties[..usize::from(caps.property_count)]
        .iter()
        .find(|cap| cap.property == PROPERTY_POWER)
        .expect("power capability");
    assert!(power.actions[..usize::from(power.action_count)].contains(&ACTION_ON));
    assert!(power.actions[..usize::from(power.action_count)].contains(&ACTION_TOGGLE));
}
