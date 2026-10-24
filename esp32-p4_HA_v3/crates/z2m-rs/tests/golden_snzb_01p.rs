//! JS <-> Rust golden check for a generated `modernExtend.battery` fragment.
//! The expected values come from z2m_spike/compile.cjs calling the real Z2M
//! converter; Rust must reproduce them from raw attribute values.

use z2m_rs::{
    decode, properties, ConfigureStep, Expose, RawZcl, Support, Unit, Update, Value, SNZB_01P,
};

#[test]
fn battery_golden_matches_zigbee2mqtt() {
    for line in include_str!("golden/snzb_01p_battery.tsv").lines() {
        if line.starts_with('#') || line.trim().is_empty() {
            continue;
        }
        let mut parts = line.split('\t');
        let attribute: u16 = parts.next().unwrap().parse().unwrap();
        let raw: u8 = parts.next().unwrap().parse().unwrap();
        let property = parts.next().unwrap();
        let expected = parts.next().unwrap();

        let event = RawZcl {
            device: [1; 8],
            endpoint: 1,
            cluster: 0x0001,
            command: 0x0A,
            attribute: Some(attribute),
            payload: &[raw],
        };
        let decoded = decode(&SNZB_01P, &event);

        if property == "-" {
            assert!(decoded.is_none(), "raw {raw} must be ignored");
            continue;
        }

        let updates: Vec<Update> = decoded.expect("decoded").updates().copied().collect();
        assert_eq!(updates.len(), 1);
        let Update::Property {
            property: id,
            value,
        } = updates[0]
        else {
            panic!("expected a property update");
        };
        let expected_id = match property {
            "battery" => properties::BATTERY,
            "voltage" => properties::VOLTAGE,
            other => panic!("unexpected property {other}"),
        };
        assert_eq!(id, expected_id);
        let actual = match value {
            Value::Float(v) => v,
            Value::Int(v) => v as f32,
            Value::Uint(v) => v as f32,
            other => panic!("unexpected value {other:?}"),
        };
        let expected: f32 = expected.parse().unwrap();
        assert!(
            (actual - expected).abs() < 0.001,
            "raw {raw}: {actual} != {expected}"
        );
    }
}

#[test]
fn battery_exposes_and_configure_are_declared() {
    let Expose::Numeric {
        property,
        unit: Unit::Percent,
        ..
    } = &SNZB_01P.exposes[0]
    else {
        panic!("battery expose");
    };
    assert_eq!(*property, properties::BATTERY);
    let Expose::Numeric {
        unit: Unit::MilliVolt,
        ..
    } = &SNZB_01P.exposes[1]
    else {
        panic!("voltage expose");
    };

    let plan = SNZB_01P.configure.expect("configure plan");
    assert_eq!(plan.timeout_ms, 5000);
    assert_eq!(plan.retries, 2);
    assert!(matches!(
        plan.steps[0],
        ConfigureStep::Report {
            cluster: 0x0001,
            attribute: 0x0021,
            min: 3600,
            max: 7200,
            change: 0,
            endpoint: 1,
        }
    ));
    assert!(matches!(
        plan.steps[1],
        ConfigureStep::Report {
            attribute: 0x0020,
            ..
        }
    ));

    assert!(SNZB_01P.support.contains(Support::IDENTIFIED));
    assert!(SNZB_01P.support.contains(Support::DECODED));
    assert!(!SNZB_01P.support.contains(Support::CONTROLLED));
}
