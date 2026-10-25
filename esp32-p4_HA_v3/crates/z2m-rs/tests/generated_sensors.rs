use z2m_rs::{
    decode_frame, properties, AttributeFrameView, Decoded, Expose, Profile, Update, Value, SENSORS,
};

fn decode(profile: &Profile, cluster: u16, payload: &[u8]) -> Vec<Update> {
    let frame = AttributeFrameView::parse(cluster, 1, 0x0A, payload).expect("frame");
    let mut out = Decoded::default();
    assert!(
        decode_frame(profile, &frame, &mut out) > 0,
        "{} did not decode",
        profile.id
    );
    out.updates().copied().collect()
}

#[test]
fn generated_sensor_profiles_decode_their_own_attributes() {
    assert!(SENSORS.len() > 500);
    let mut checked = 0;
    for profile in SENSORS {
        for expose in profile.exposes {
            match expose {
                Expose::Numeric { property, .. } if *property == properties::BATTERY => {
                    assert_eq!(
                        decode(profile, 0x0001, &[0x21, 0x00, 0x20, 175])[0],
                        Update::Property {
                            property: *property,
                            value: Value::Float(87.5)
                        }
                    );
                    checked += 1;
                }
                Expose::Numeric { property, .. } if *property == properties::TEMPERATURE => {
                    assert_eq!(
                        decode(profile, 0x0402, &[0x00, 0x00, 0x29, 0x2c, 0x01])[0],
                        Update::Property {
                            property: *property,
                            value: Value::Float(3.0)
                        }
                    );
                    checked += 1;
                }
                Expose::Numeric { property, .. } if *property == properties::HUMIDITY => {
                    assert_eq!(
                        decode(profile, 0x0405, &[0x00, 0x00, 0x21, 0x2c, 0x01])[0],
                        Update::Property {
                            property: *property,
                            value: Value::Float(3.0)
                        }
                    );
                    checked += 1;
                }
                Expose::Numeric { property, .. } if *property == properties::ILLUMINANCE => {
                    let lux = 10f32.powf((5000.0 - 1.0) / 10000.0);
                    assert_eq!(
                        decode(profile, 0x0400, &[0x00, 0x00, 0x21, 0x88, 0x13])[0],
                        Update::Property {
                            property: *property,
                            value: Value::Float(lux)
                        }
                    );
                    checked += 1;
                }
                Expose::Binary { property } if *property == properties::OCCUPANCY => {
                    assert_eq!(
                        decode(profile, 0x0406, &[0x00, 0x00, 0x18, 0x01])[0],
                        Update::Property {
                            property: *property,
                            value: Value::Bool(true)
                        }
                    );
                    checked += 1;
                }
                _ => {}
            }
        }
    }
    assert!(checked > 500, "only {checked} sensor attributes checked");
}
