use ha_domain::canonical::{
    PROPERTY_BATTERY_PERCENT, PROPERTY_BATTERY_VOLTAGE, PROPERTY_HUMIDITY, PROPERTY_ILLUMINANCE,
    PROPERTY_OCCUPANCY, PROPERTY_TEMPERATURE,
};
use ha_domain::Value as DomainValue;
use zigbee_devices::{canonical_states, profile_capabilities};
use z2m_rs::{properties, Expose, Fingerprint, Manufacturer, Profile, Support, Unit, Update, Value};

static SENSOR: Profile = Profile {
    id: "test.sensor",
    vendor: "test",
    fingerprints: &[Fingerprint {
        manufacturer: Manufacturer::Any,
        model: "SENSOR",
    }],
    exposes: &[
        Expose::Numeric { property: properties::BATTERY, unit: Unit::Percent, min: 0.0, max: 100.0, step: 1.0 },
        Expose::Numeric { property: properties::VOLTAGE, unit: Unit::MilliVolt, min: 0.0, max: 5000.0, step: 1.0 },
        Expose::Numeric { property: properties::TEMPERATURE, unit: Unit::Celsius, min: -40.0, max: 125.0, step: 0.1 },
        Expose::Numeric { property: properties::HUMIDITY, unit: Unit::Percent, min: 0.0, max: 100.0, step: 1.0 },
        Expose::Numeric { property: properties::ILLUMINANCE, unit: Unit::Lux, min: 0.0, max: 100000.0, step: 1.0 },
        Expose::Binary { property: properties::OCCUPANCY },
    ],
    from: &[],
    to: &[],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

fn state(update: Update) -> (u32, DomainValue) {
    let (states, event) = canonical_states(1, &update);
    assert!(event.is_none());
    let state = states[0].expect("state");
    (state.property, state.value)
}

#[test]
fn sensor_capabilities_use_v2_ids() {
    let caps = profile_capabilities(&SENSOR);
    let properties = &caps.properties[..usize::from(caps.property_count)];
    for expected in [
        PROPERTY_BATTERY_PERCENT,
        PROPERTY_BATTERY_VOLTAGE,
        PROPERTY_TEMPERATURE,
        PROPERTY_HUMIDITY,
        PROPERTY_ILLUMINANCE,
        PROPERTY_OCCUPANCY,
    ] {
        assert!(properties.iter().any(|capability| capability.property == expected));
    }
}

#[test]
fn sensor_updates_use_v2_ids_and_units() {
    assert_eq!(
        state(Update::Property { property: properties::BATTERY, value: Value::Float(87.5) }),
        (PROPERTY_BATTERY_PERCENT, DomainValue::f32(87.5))
    );
    assert_eq!(
        state(Update::Property { property: properties::VOLTAGE, value: Value::Float(3000.0) }),
        (PROPERTY_BATTERY_VOLTAGE, DomainValue::f32(3.0))
    );
    assert_eq!(
        state(Update::Property { property: properties::TEMPERATURE, value: Value::Float(21.5) }),
        (PROPERTY_TEMPERATURE, DomainValue::f32(21.5))
    );
    assert_eq!(
        state(Update::Property { property: properties::HUMIDITY, value: Value::Float(45.0) }),
        (PROPERTY_HUMIDITY, DomainValue::f32(45.0))
    );
    assert_eq!(
        state(Update::Property { property: properties::ILLUMINANCE, value: Value::Float(123.0) }),
        (PROPERTY_ILLUMINANCE, DomainValue::f32(123.0))
    );
    assert_eq!(
        state(Update::Property { property: properties::OCCUPANCY, value: Value::Bool(true) }),
        (PROPERTY_OCCUPANCY, DomainValue::bool(true))
    );
}