use z2m_rs::capabilities::{
    BATTERY_ATTR, BATTERY_VOLTAGE_ATTR, HUMIDITY_ATTR, ILLUMINANCE_ATTR, OCCUPANCY_ATTR,
    TEMPERATURE_ATTR,
};
use z2m_rs::{
    decode_frame, AttrType, AttributeFrameView, AttributeRule, Converter, Decoded, Profile, Scale,
    Update, Value,
};

static NODIVIDE: &[Converter] = &[Converter::Attribute(AttributeRule {
    cluster: 0x0001,
    attribute: 0x0021,
    kind: AttrType::Uint8,
    property: z2m_rs::properties::BATTERY,
    scale: Scale { mul: 1, div: 1 },
    skip: Some(255),
})];

static BATTERY: &[Converter] = &[BATTERY_ATTR];
static VOLTAGE: &[Converter] = &[BATTERY_VOLTAGE_ATTR];
static TEMPERATURE: &[Converter] = &[TEMPERATURE_ATTR];
static HUMIDITY: &[Converter] = &[HUMIDITY_ATTR];
static OCCUPANCY: &[Converter] = &[OCCUPANCY_ATTR];
static ILLUMINANCE: &[Converter] = &[ILLUMINANCE_ATTR];

fn profile(kind: &str) -> Profile {
    let from = match kind {
        "battery" => BATTERY,
        "voltage" => VOLTAGE,
        "battery_nodivide" => NODIVIDE,
        "temperature" => TEMPERATURE,
        "humidity" => HUMIDITY,
        "occupancy" => OCCUPANCY,
        "illuminance" => ILLUMINANCE,
        other => panic!("unknown kind {other}"),
    };
    Profile {
        id: "test.sensor-differential",
        vendor: "test",
        fingerprints: &[],
        exposes: &[],
        from,
        to: &[],
        configure: None,
        omit_optional_params: false,
        turns_off_at_brightness_1: false,
        support: z2m_rs::Support::IDENTIFIED,
    }
}

#[test]
fn sensor_converters_match_zhc_javascript() {
    let data = include_str!("golden/sensor_conversions.tsv");
    let mut checked = 0;
    for line in data.lines().skip(1) {
        let mut fields = line.split('\t');
        let kind = fields.next().expect("kind");
        let cluster = u16::from_str_radix(fields.next().expect("cluster"), 16).expect("cluster");
        let payload: Vec<u8> = (0..fields.next().expect("payload").len())
            .step_by(2)
            .map(|index| {
                u8::from_str_radix(&line.split('\t').nth(2).unwrap()[index..index + 2], 16).unwrap()
            })
            .collect();
        let expected = fields.next().expect("expected");
        let frame = AttributeFrameView::parse(cluster, 1, 0x0A, &payload).expect("frame");
        let mut out = Decoded::default();
        let count = decode_frame(&profile(kind), &frame, &mut out);
        if expected == "none" {
            assert_eq!(count, 0, "{kind} should be ignored");
            continue;
        }
        assert_eq!(count, 1, "{kind} should decode");
        let update = out.updates().next().expect("update");
        if let Some(value) = expected.strip_prefix("f32:") {
            let wanted: f32 = value.parse().unwrap();
            let actual = match update {
                Update::Property {
                    value: Value::Float(value),
                    ..
                } => *value,
                Update::Property {
                    value: Value::Int(value),
                    ..
                } => *value as f32,
                _ => panic!("{kind}: expected number, got {update:?}"),
            };
            assert!(
                (actual - wanted).abs() < 1e-5,
                "{kind}: {actual} != {wanted}"
            );
        } else if let Some(value) = expected.strip_prefix("bool:") {
            let wanted = value == "true";
            assert_eq!(
                update,
                &Update::Property {
                    property: z2m_rs::properties::OCCUPANCY,
                    value: Value::Bool(wanted)
                }
            );
        } else {
            panic!("unknown expected {expected}");
        }
        checked += 1;
    }
    assert!(checked >= 10);
}
