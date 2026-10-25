use z2m_rs::capabilities::{
    BATTERY_ATTR, BATTERY_VOLTAGE_ATTR, HUMIDITY_ATTR, ILLUMINANCE_ATTR, OCCUPANCY_ATTR,
    TEMPERATURE_ATTR,
};
use z2m_rs::{decode_frame, AttributeFrameView, Converter, Profile, Support, Update, Value};

static PROFILE: Profile = Profile {
    id: "test.sensor-mechanisms",
    vendor: "test",
    fingerprints: &[],
    exposes: &[],
    from: &[
        BATTERY_ATTR,
        BATTERY_VOLTAGE_ATTR,
        TEMPERATURE_ATTR,
        HUMIDITY_ATTR,
        OCCUPANCY_ATTR,
        ILLUMINANCE_ATTR,
    ],
    to: &[],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

fn decode(cluster: u16, payload: &[u8]) -> Vec<Update> {
    let frame = AttributeFrameView::parse(cluster, 1, 0x0A, payload).unwrap();
    let mut out = z2m_rs::Decoded::default();
    assert_eq!(decode_frame(&PROFILE, &frame, &mut out), 1);
    out.updates().copied().collect()
}

#[test]
fn standard_sensor_mechanisms_match_zhc_units() {
    assert_eq!(
        decode(0x0001, &[0x21, 0x00, 0x20, 175]),
        vec![Update::Property {
            property: z2m_rs::properties::BATTERY,
            value: Value::Float(87.5)
        }]
    );
    assert_eq!(
        decode(0x0001, &[0x20, 0x00, 0x20, 30]),
        vec![Update::Property {
            property: z2m_rs::properties::VOLTAGE,
            value: Value::Float(3000.0)
        }]
    );
    assert_eq!(
        decode(0x0402, &[0x00, 0x00, 0x29, 0x00, 0x08]),
        vec![Update::Property {
            property: z2m_rs::properties::TEMPERATURE,
            value: Value::Float(20.48)
        }]
    );
    assert_eq!(
        decode(0x0405, &[0x00, 0x00, 0x21, 0x2c, 0x01]),
        vec![Update::Property {
            property: z2m_rs::properties::HUMIDITY,
            value: Value::Float(3.0)
        }]
    );
    assert_eq!(
        decode(0x0406, &[0x00, 0x00, 0x18, 0x01]),
        vec![Update::Property {
            property: z2m_rs::properties::OCCUPANCY,
            value: Value::Bool(true)
        }]
    );
    let lux = 10f32.powf((5000.0 - 1.0) / 10000.0);
    assert_eq!(
        decode(0x0400, &[0x00, 0x00, 0x21, 0x88, 0x13]),
        vec![Update::Property {
            property: z2m_rs::properties::ILLUMINANCE,
            value: Value::Float(lux)
        }]
    );
}
