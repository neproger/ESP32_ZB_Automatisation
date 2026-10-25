//! Thin Zigbee → canonical HA mapping. No Domain storage here: this module
//! derives canonical ids, projects capabilities and translates values/commands.

use ha_domain::canonical::{
    device_id_derive, CapabilitiesRecord, Capability, CanonicalCommand, CommandValue, EntityId,
    EventId, PropertyId, ACTION_OFF, ACTION_ON, ACTION_SET, EVENT_DOUBLE_PRESS, EVENT_HOLD,
    EVENT_SINGLE_PRESS, PROPERTY_BRIGHTNESS, PROPERTY_COLOR, PROPERTY_COLOR_HUE,
    PROPERTY_COLOR_SATURATION, PROPERTY_COLOR_TEMPERATURE, PROPERTY_COLOR_X, PROPERTY_COLOR_Y,
    PROPERTY_POWER, SOURCE_ZIGBEE,
};
use ha_domain::Value as DomainValue;
use z2m_rs::{
    properties, ColorValue, Command as Z2mCommand, CommandCodec, Expose, Profile, Update, Value,
};

pub fn zigbee_entity_id(uid: u64, endpoint: u8) -> EntityId {
    ha_domain::canonical::zigbee_entity_id(uid, endpoint)
}

pub fn zigbee_device_id(uid: u64) -> ha_domain::canonical::DeviceId {
    let native = format!("{uid:016x}");
    device_id_derive(SOURCE_ZIGBEE, native.as_bytes())
}

pub fn profile_capabilities(profile: &Profile) -> CapabilitiesRecord {
    let mut record = CapabilitiesRecord::default();
    let has_xy = profile.to.iter().any(|map| map.codec == CommandCodec::ColorXy);
    for expose in profile.exposes {
        match expose {
            Expose::Binary { property } if *property == properties::STATE => {
                record.push(Capability::with_actions(
                    PROPERTY_POWER,
                    &[ACTION_ON, ACTION_OFF],
                ));
            }
            Expose::Numeric { property, .. } if *property == properties::BRIGHTNESS => {
                record.push(Capability::with_actions(PROPERTY_BRIGHTNESS, &[ACTION_SET]));
            }
            Expose::Numeric { property, .. } if *property == properties::COLOR_TEMP => {
                record.push(Capability::with_actions(
                    PROPERTY_COLOR_TEMPERATURE,
                    &[ACTION_SET],
                ));
            }
            Expose::Color { .. } if has_xy => {
                record.push(Capability::with_actions(PROPERTY_COLOR, &[ACTION_SET]));
            }
            Expose::Action(name) => {
                if let Some(event) = event_for(name) {
                    let already = record.events[..usize::from(record.event_count)]
                        .contains(&event);
                    if !already {
                        record.push_event(event);
                    }
                }
            }
            Expose::Composite(inner) => {
                for expose in *inner {
                    if let Expose::Action(name) = expose {
                        if let Some(event) = event_for(name) {
                            let already = record.events[..usize::from(record.event_count)]
                                .contains(&event);
                            if !already {
                                record.push_event(event);
                            }
                        }
                    }
                }
            }
            _ => {}
        }
    }
    record
}

fn event_for(name: &str) -> Option<EventId> {
    match name {
        "single" => Some(EVENT_SINGLE_PRESS),
        "double" => Some(EVENT_DOUBLE_PRESS),
        "hold" => Some(EVENT_HOLD),
        _ => None,
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StateUpdate {
    pub entity: EntityId,
    pub property: PropertyId,
    pub value: DomainValue,
}

/// One z2m update can produce up to two scalar states (XY or HS) or an event.
pub fn canonical_states(
    entity: EntityId,
    update: &Update,
) -> ([Option<StateUpdate>; 2], Option<EventId>) {
    let mut states = [None; 2];
    let event = match update {
        Update::Property { property, value } if *property == properties::STATE => {
            if let Value::Bool(on) = value {
                states[0] = Some(StateUpdate {
                    entity,
                    property: PROPERTY_POWER,
                    value: DomainValue::bool(*on),
                });
            }
            None
        }
        Update::Property { property, value } if *property == properties::BRIGHTNESS => {
            if let Some(raw) = numeric(value) {
                states[0] = Some(StateUpdate {
                    entity,
                    property: PROPERTY_BRIGHTNESS,
                    value: DomainValue::f32(raw * 100.0 / 254.0),
                });
            }
            None
        }
        Update::Property { property, value } if *property == properties::COLOR_TEMP => {
            if let Some(mireds) = numeric(value) {
                if mireds > 0.0 {
                    states[0] = Some(StateUpdate {
                        entity,
                        property: PROPERTY_COLOR_TEMPERATURE,
                        value: DomainValue::f32(1_000_000.0 / mireds),
                    });
                }
            }
            None
        }
        Update::Property { property, value } if *property == properties::COLOR => {
            if let Value::Color(color) = value {
                match color {
                    ColorValue::Xy {
                        x: Some(x),
                        y: Some(y),
                    } => {
                        states[0] = Some(StateUpdate {
                            entity,
                            property: PROPERTY_COLOR_X,
                            value: DomainValue::f32(*x),
                        });
                        states[1] = Some(StateUpdate {
                            entity,
                            property: PROPERTY_COLOR_Y,
                            value: DomainValue::f32(*y),
                        });
                    }
                    ColorValue::Hs {
                        hue: Some(hue),
                        saturation: Some(saturation),
                    } => {
                        states[0] = Some(StateUpdate {
                            entity,
                            property: PROPERTY_COLOR_HUE,
                            value: DomainValue::f32(*hue),
                        });
                        states[1] = Some(StateUpdate {
                            entity,
                            property: PROPERTY_COLOR_SATURATION,
                            value: DomainValue::f32(*saturation),
                        });
                    }
                    _ => {}
                }
            }
            None
        }
        Update::Action(action) => Some(match action {
            z2m_rs::Action::Single => EVENT_SINGLE_PRESS,
            z2m_rs::Action::Double => EVENT_DOUBLE_PRESS,
            z2m_rs::Action::Hold => EVENT_HOLD,
        }),
        _ => None,
    };
    (states, event)
}

fn numeric(value: &Value) -> Option<f32> {
    match value {
        Value::Int(v) => Some(*v as f32),
        Value::Float(v) => Some(*v),
        Value::Uint(v) => Some(*v as f32),
        Value::Enum(v) => Some(*v as f32),
        Value::Bool(v) => Some(u8::from(*v) as f32),
        Value::Color(_) => None,
    }
}

fn scalar(value: &CommandValue) -> Option<f32> {
    let CommandValue::Scalar(value) = value else {
        return None;
    };
    value.as_f32().or_else(|| value.as_i32().map(|v| v as f32))
        .or_else(|| value.as_u32().map(|v| v as f32))
}

pub fn canonical_command(_profile: &Profile, request: &CanonicalCommand) -> Option<Z2mCommand> {
    if request.property == PROPERTY_POWER {
        return match request.action {
            ACTION_ON => Some(Z2mCommand::Property {
                property: properties::STATE,
                value: Value::Bool(true),
            }),
            ACTION_OFF => Some(Z2mCommand::Property {
                property: properties::STATE,
                value: Value::Bool(false),
            }),
            _ => None,
        };
    }
    if request.property == PROPERTY_BRIGHTNESS && request.action == ACTION_SET {
        let percent = scalar(&request.value)?;
        let level = (percent.clamp(0.0, 100.0) * 254.0 / 100.0).round() as i32;
        return Some(Z2mCommand::Property {
            property: properties::BRIGHTNESS,
            value: Value::Int(level),
        });
    }
    if request.property == PROPERTY_COLOR_TEMPERATURE && request.action == ACTION_SET {
        let kelvin = scalar(&request.value)?;
        if kelvin <= 0.0 {
            return None;
        }
        let mireds = (1_000_000.0 / kelvin).round().clamp(0.0, 65535.0) as i32;
        return Some(Z2mCommand::Property {
            property: properties::COLOR_TEMP,
            value: Value::Int(mireds),
        });
    }
    if request.property == PROPERTY_COLOR && request.action == ACTION_SET {
        if let CommandValue::Xy { x, y } = request.value {
            return Some(Z2mCommand::Property {
                property: properties::COLOR,
                value: Value::Color(ColorValue::xy(x, y)),
            });
        }
    }
    None
}

pub fn encode_canonical_command(
    profile: &Profile,
    request: &CanonicalCommand,
    out: &mut [u8],
) -> Option<(u16, u8, usize)> {
    let command = canonical_command(profile, request)?;
    let (cluster, zcl_command) = z2m_rs::resolve_command(profile, &command)?;
    let len = z2m_rs::encode(profile, &command, out)?;
    Some((cluster, zcl_command, len))
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::canonical::{
        ACTION_ON, ACTION_SET, PROPERTY_BRIGHTNESS, PROPERTY_COLOR_TEMPERATURE, PROPERTY_POWER,
    };
    use z2m_rs::capabilities::{
        COLORTEMP_TO, DIM_CT_EXPOSES, LEVEL_TO, ONOFF_OFF, ONOFF_ON,
    };
    use z2m_rs::{Fingerprint, Manufacturer, Profile, Support};

    static PROFILE: Profile = Profile {
        id: "test.canonical",
        vendor: "test",
        fingerprints: &[Fingerprint {
            manufacturer: Manufacturer::Any,
            model: "TEST",
        }],
        exposes: DIM_CT_EXPOSES,
        from: &[],
        to: &[ONOFF_ON, ONOFF_OFF, LEVEL_TO, COLORTEMP_TO],
        configure: None,
        omit_optional_params: false,
        turns_off_at_brightness_1: false,
        support: Support::IDENTIFIED,
    };

    #[test]
    fn identity_is_deterministic_and_matches_v2() {
        let uid = 0x0012_4B00_0A1B_2C3D;
        assert_eq!(zigbee_entity_id(uid, 1), 0xdf80_3cba_f588_876d);
        assert_ne!(zigbee_entity_id(uid, 1), zigbee_entity_id(uid, 2));
        assert_eq!(zigbee_device_id(uid), zigbee_device_id(uid));
        assert_ne!(zigbee_device_id(uid), zigbee_device_id(uid + 1));
    }

    #[test]
    fn capabilities_map_profile_properties() {
        let caps = profile_capabilities(&PROFILE);
        assert_eq!(caps.property_count, 3);
        assert!(caps.properties[..3].iter().any(|c| c.property == PROPERTY_POWER));
        assert!(caps.properties[..3]
            .iter()
            .any(|c| c.property == PROPERTY_BRIGHTNESS));
        assert!(caps.properties[..3]
            .iter()
            .any(|c| c.property == PROPERTY_COLOR_TEMPERATURE));
        assert_eq!(caps.encode().len(), ha_domain::canonical::CAPABILITIES_LEN);
    }

    #[test]
    fn state_values_use_canonical_units() {
        let entity = 7;
        let (states, event) = canonical_states(
            entity,
            &Update::Property {
                property: properties::BRIGHTNESS,
                value: Value::Int(127),
            },
        );
        assert!(event.is_none());
        assert_eq!(states[0].unwrap().value.as_f32().unwrap(), 50.0);

        let (states, _) = canonical_states(
            entity,
            &Update::Property {
                property: properties::COLOR_TEMP,
                value: Value::Uint(250),
            },
        );
        assert_eq!(states[0].unwrap().value.as_f32().unwrap(), 4000.0);

        let (states, _) = canonical_states(
            entity,
            &Update::Property {
                property: properties::COLOR,
                value: Value::Color(ColorValue::xy(0.5, 0.25)),
            },
        );
        assert_eq!(states[0].unwrap().value.as_f32().unwrap(), 0.5);
        assert_eq!(states[1].unwrap().value.as_f32().unwrap(), 0.25);
    }

    #[test]
    fn commands_convert_and_encode() {
        let mut out = [0u8; 16];
        let power = CanonicalCommand {
            entity: 1,
            property: PROPERTY_POWER,
            action: ACTION_ON,
            value: CommandValue::None,
        };
        assert_eq!(encode_canonical_command(&PROFILE, &power, &mut out), Some((0x0006, 0x01, 0)));

        let brightness = CanonicalCommand {
            entity: 1,
            property: PROPERTY_BRIGHTNESS,
            action: ACTION_SET,
            value: CommandValue::Scalar(DomainValue::f32(50.0)),
        };
        let (cluster, command, len) =
            encode_canonical_command(&PROFILE, &brightness, &mut out).unwrap();
        assert_eq!((cluster, command), (0x0008, 0x04));
        assert_eq!(&out[..len], &[127, 0, 0, 0, 0]);

        let ct = CanonicalCommand {
            entity: 1,
            property: PROPERTY_COLOR_TEMPERATURE,
            action: ACTION_SET,
            value: CommandValue::Scalar(DomainValue::f32(4000.0)),
        };
        let (_, _, len) = encode_canonical_command(&PROFILE, &ct, &mut out).unwrap();
        assert_eq!(&out[..len], &[250, 0, 0, 0, 0, 0]);
    }
}
