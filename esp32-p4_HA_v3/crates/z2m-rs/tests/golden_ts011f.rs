//! JS <-> Rust golden checks for the generated `tuyaOnOff` core fragment.
//! State comes from standard genOnOff attribute reports; writes are ZCL On/Off.

use z2m_rs::{
    decode, properties, resolve_command, Command, ConfigureEndpoint, ConfigureStep, RawZcl,
    Support, Update, Value, TS011F, TUYA_MAGIC_ATTRIBUTES,
};

fn event(attribute: u16, payload: &[u8]) -> RawZcl<'_> {
    RawZcl {
        device: [1; 8],
        endpoint: 1,
        cluster: 0x0006,
        command: 0x0A,
        attribute: Some(attribute),
        payload,
    }
}

#[test]
fn onoff_state_matches_zigbee2mqtt() {
    for line in include_str!("golden/ts011f_onoff.tsv").lines() {
        if line.starts_with('#') || line.trim().is_empty() {
            continue;
        }
        let mut parts = line.split('\t');
        let attribute: u16 = parts.next().unwrap().parse().unwrap();
        let raw: u8 = parts.next().unwrap().parse().unwrap();
        let state = parts.next().unwrap();

        let decoded = decode(&TS011F, &event(attribute, &[raw])).expect("decoded");
        let updates: Vec<Update> = decoded.updates().copied().collect();
        assert_eq!(updates.len(), 1);
        let Update::Property {
            property,
            value,
        } = updates[0]
        else {
            panic!("expected property");
        };
        assert_eq!(property, properties::STATE);
        assert_eq!(value, Value::Bool(state == "ON"));
    }
}

#[test]
fn onoff_commands_match_zcl() {
    for line in include_str!("golden/ts011f_commands.tsv").lines() {
        if line.starts_with('#') || line.trim().is_empty() {
            continue;
        }
        let mut parts = line.split('\t');
        let state = parts.next().unwrap();
        let zcl: u8 = parts.next().unwrap().parse().unwrap();
        let command = Command::Property {
            property: properties::STATE,
            value: Value::Bool(state == "ON"),
        };
        assert_eq!(resolve_command(&TS011F, &command), Some((0x0006, zcl)));
    }
}

#[test]
fn plug_configures_magic_packet_bind_and_onoff_reporting() {
    let plan = TS011F.configure.expect("configure plan");
    assert_eq!(plan.version, 1);
    assert!(matches!(
        plan.steps[0],
        ConfigureStep::Read {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0000,
            attributes,
        } if attributes == TUYA_MAGIC_ATTRIBUTES
    ));
    assert!(matches!(
        plan.steps[1],
        ConfigureStep::Bind {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
        }
    ));
    assert!(matches!(
        plan.steps[2],
        ConfigureStep::Report {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
            attribute: 0x0000,
            attribute_type: 0x10,
            min: 0,
            max: 3600,
            change: 1,
        }
    ));
}

#[test]
fn tuya_on_off_is_partial() {
    assert!(TS011F.support.contains(Support::DECODED));
    assert!(TS011F.support.contains(Support::CONTROLLED));
    assert!(TS011F.support.contains(Support::PARTIAL));
}
