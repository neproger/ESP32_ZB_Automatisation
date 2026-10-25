//! Shared Zigbee capabilities. Profiles compose these fragments; they do not
//! describe device families or models.

use crate::properties::{
    BATTERY, BRIGHTNESS, COLOR, COLOR_MODE, COLOR_TEMP, HUMIDITY, OCCUPANCY, STATE, TEMPERATURE,
    VOLTAGE,
};
use crate::{
    AttrType, AttributeFrameView, AttributeRule, ColorValue, Command, CommandCodec, CommandMap,
    ConfigureEndpoint, ConfigurePlan, ConfigureStep, Converter, Decoded, Expose, Scale, Unit,
    Update, Value,
};

pub const ONOFF_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0006,
    attribute: 0x0000,
    kind: AttrType::Bool,
    property: STATE,
    scale: Scale::NONE,
    skip: None,
});

pub const BATTERY_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0001,
    attribute: 0x0021,
    kind: AttrType::Uint8,
    property: BATTERY,
    scale: Scale { mul: 1, div: 2 },
    skip: Some(255),
});

pub const BATTERY_VOLTAGE_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0001,
    attribute: 0x0020,
    kind: AttrType::Uint8,
    property: VOLTAGE,
    scale: Scale { mul: 100, div: 1 },
    skip: Some(255),
});

pub const TEMPERATURE_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0402,
    attribute: 0x0000,
    kind: AttrType::Int16,
    property: TEMPERATURE,
    scale: Scale { mul: 1, div: 100 },
    skip: None,
});

pub const HUMIDITY_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0405,
    attribute: 0x0000,
    kind: AttrType::Uint16,
    property: HUMIDITY,
    scale: Scale { mul: 1, div: 100 },
    skip: None,
});

pub const OCCUPANCY_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0406,
    attribute: 0x0000,
    kind: AttrType::Bitmap8,
    property: OCCUPANCY,
    scale: Scale::NONE,
    skip: None,
});

pub const ILLUMINANCE_ATTR: Converter = Converter::Illuminance {
    cluster: 0x0400,
    attribute: 0x0000,
};

pub const ONOFF_ON: CommandMap = CommandMap {
    command: Command::Property {
        property: STATE,
        value: Value::Bool(true),
    },
    cluster: 0x0006,
    zcl_command: 0x01,
    args: &[],
    codec: CommandCodec::None,
};

pub const ONOFF_OFF: CommandMap = CommandMap {
    command: Command::Property {
        property: STATE,
        value: Value::Bool(false),
    },
    cluster: 0x0006,
    zcl_command: 0x00,
    args: &[],
    codec: CommandCodec::None,
};

pub const LEVEL_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0008,
    attribute: 0x0000,
    kind: AttrType::Uint8,
    property: BRIGHTNESS,
    scale: Scale::NONE,
    skip: None,
});

/// Z2M `brightness` is 0..254; `moveToLevelWithOnOff` carries level + transition.
pub const LEVEL_TO: CommandMap = CommandMap {
    command: Command::Property {
        property: BRIGHTNESS,
        value: Value::Int(0),
    },
    cluster: 0x0008,
    zcl_command: 0x04,
    args: &[],
    codec: CommandCodec::Level,
};

pub const LEVEL_TO_NO_ONOFF: CommandMap = CommandMap {
    command: Command::Property {
        property: BRIGHTNESS,
        value: Value::Int(0),
    },
    cluster: 0x0008,
    zcl_command: 0x00,
    args: &[],
    codec: CommandCodec::Level,
};

pub const COLORTEMP_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0300,
    attribute: 0x0007,
    kind: AttrType::Uint16,
    property: COLOR_TEMP,
    scale: Scale::NONE,
    skip: None,
});

pub const COLORTEMP_TO: CommandMap = CommandMap {
    command: Command::Property {
        property: COLOR_TEMP,
        value: Value::Int(0),
    },
    cluster: 0x0300,
    zcl_command: 0x0A,
    args: &[],
    codec: CommandCodec::ColorTemp,
};

pub static DIM_EXPOSES: &[Expose] = &[
    Expose::Binary { property: STATE },
    Expose::Numeric {
        property: BRIGHTNESS,
        unit: Unit::None,
        min: 0.0,
        max: 254.0,
        step: 1.0,
    },
];

pub static DIM_FROM: &[Converter] = &[ONOFF_ATTR, LEVEL_ATTR];
pub static DIM_TO: &[CommandMap] = &[ONOFF_ON, ONOFF_OFF, LEVEL_TO];

pub static DIM_CONFIGURE: ConfigurePlan = ConfigurePlan {
    steps: &[
        ConfigureStep::Bind {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
        },
        ConfigureStep::Bind {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0008,
        },
        ConfigureStep::Report {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
            attribute: 0x0000,
            attribute_type: 0x10,
            min: 0,
            max: 3600,
            change: 1,
        },
        ConfigureStep::Report {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0008,
            attribute: 0x0000,
            attribute_type: 0x20,
            min: 0,
            max: 3600,
            change: 1,
        },
    ],
    timeout_ms: 30_000,
    retries: 3,
    version: 1,
};

pub static DIM_CT_EXPOSES: &[Expose] = &[
    Expose::Binary { property: STATE },
    Expose::Numeric {
        property: BRIGHTNESS,
        unit: Unit::None,
        min: 0.0,
        max: 254.0,
        step: 1.0,
    },
    Expose::Numeric {
        property: COLOR_TEMP,
        unit: Unit::Mired,
        min: 153.0,
        max: 500.0,
        step: 1.0,
    },
];

pub static DIM_CT_FROM: &[Converter] = &[ONOFF_ATTR, LEVEL_ATTR, COLOR_FRAME];
pub static DIM_CT_TO: &[CommandMap] = &[ONOFF_ON, ONOFF_OFF, LEVEL_TO, COLORTEMP_TO];

pub static DIM_CT_CONFIGURE: ConfigurePlan = ConfigurePlan {
    steps: &[
        ConfigureStep::Bind {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
        },
        ConfigureStep::Bind {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0008,
        },
        ConfigureStep::Bind {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0300,
        },
        ConfigureStep::Report {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
            attribute: 0x0000,
            attribute_type: 0x10,
            min: 0,
            max: 3600,
            change: 1,
        },
        ConfigureStep::Report {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0008,
            attribute: 0x0000,
            attribute_type: 0x20,
            min: 0,
            max: 3600,
            change: 1,
        },
        ConfigureStep::Report {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0300,
            attribute: 0x0007,
            attribute_type: 0x21,
            min: 0,
            max: 3600,
            change: 1,
        },
        ConfigureStep::Read {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0300,
            attributes: &[0x4000, 0x4001, 0x4002],
        },
    ],
    timeout_ms: 30_000,
    retries: 3,
    version: 1,
};

/// ZHC light.configure reads color capabilities and the physical mired range;
/// without configureReporting it performs no binding or reporting.
pub static DIM_CT_READ: ConfigurePlan = ConfigurePlan {
    steps: &[ConfigureStep::Read {
        endpoint: ConfigureEndpoint::First,
        cluster: 0x0300,
        attributes: &[0x4000, 0x4001, 0x4002],
    }],
    timeout_ms: 30_000,
    retries: 3,
    version: 1,
};

/// Assembles color from the whole ZCL frame, matching `fz.color_colortemp`.
fn decode_color(frame: &AttributeFrameView<'_>, decoded: &mut Decoded) -> bool {
    if frame.cluster != 0x0300 {
        return false;
    }
    let mut x = None;
    let mut y = None;
    let mut hue = None;
    let mut saturation = None;
    let mut enhanced_hue = None;
    let mut mode = None;
    let mut color_temp = None;
    for attr in frame.attributes() {
        let value = attr.value;
        match attr.id {
            0x0000 => hue = value.first().map(|raw| f32::from(*raw) * 360.0 / 254.0),
            0x0001 => saturation = value.first().map(|raw| f32::from(*raw) * 100.0 / 254.0),
            0x0003 => x = u16_le(value).map(|raw| f32::from(raw) / 65535.0),
            0x0004 => y = u16_le(value).map(|raw| f32::from(raw) / 65535.0),
            0x0007 => color_temp = u16_le(value),
            0x0008 => mode = value.first().copied(),
            0x4000 => enhanced_hue = u16_le(value).map(|raw| f32::from(raw) * 360.0 / 65535.0),
            _ => {}
        }
    }

    let mut any = false;
    if let Some(raw) = color_temp {
        any |= decoded.push(Update::Property {
            property: COLOR_TEMP,
            value: Value::Uint(u32::from(raw)),
        });
    }
    let mode = mode.or_else(|| color_temp.map(|_| 2));
    if let Some(raw) = mode {
        any |= decoded.push(Update::Property {
            property: COLOR_MODE,
            value: Value::Enum(u32::from(raw)),
        });
    }

    let hue = enhanced_hue.or(hue);
    let color = if x.is_some() || y.is_some() {
        match mode {
            Some(0) if hue.is_some() || saturation.is_some() => {
                Some(ColorValue::Hs { hue, saturation })
            }
            _ => Some(ColorValue::Xy { x, y }),
        }
    } else if hue.is_some() || saturation.is_some() {
        Some(ColorValue::Hs { hue, saturation })
    } else {
        None
    };
    if let Some(color) = color {
        any |= decoded.push(Update::Property {
            property: COLOR,
            value: Value::Color(color),
        });
    }
    any
}

fn u16_le(bytes: &[u8]) -> Option<u16> {
    (bytes.len() >= 2).then(|| u16::from_le_bytes([bytes[0], bytes[1]]))
}

pub const COLOR_FRAME: Converter = Converter::Frame(decode_color);

pub const COLOR_XY_TO: CommandMap = CommandMap {
    command: Command::Property {
        property: COLOR,
        value: Value::Color(ColorValue::Xy {
            x: Some(0.0),
            y: Some(0.0),
        }),
    },
    cluster: 0x0300,
    zcl_command: 0x07,
    args: &[],
    codec: CommandCodec::ColorXy,
};

pub const COLOR_HS_TO: CommandMap = CommandMap {
    command: Command::Property {
        property: COLOR,
        value: Value::Color(ColorValue::Hs {
            hue: Some(0.0),
            saturation: Some(0.0),
        }),
    },
    cluster: 0x0300,
    zcl_command: 0x06,
    args: &[],
    codec: CommandCodec::ColorHs,
};

pub const COLOR_ENHANCED_TO: CommandMap = CommandMap {
    command: Command::Property {
        property: COLOR,
        value: Value::Color(ColorValue::Hs {
            hue: Some(0.0),
            saturation: Some(0.0),
        }),
    },
    cluster: 0x0300,
    zcl_command: 0x43,
    args: &[],
    codec: CommandCodec::ColorHsEnhanced,
};

#[cfg(test)]
mod tests {
    use super::*;
    use crate::properties::{BRIGHTNESS, COLOR, COLOR_MODE, COLOR_TEMP};
    use crate::{
        decode, encode, AttributeFrameView, ColorValue, Command, Fingerprint, Manufacturer,
        Profile, RawZcl, Support, Update, Value,
    };

    static FINGERPRINTS: &[Fingerprint] = &[Fingerprint {
        manufacturer: Manufacturer::Any,
        model: "DIM-CT-TEST",
    }];

    static PROFILE: Profile = Profile {
        id: "test.dimct",
        vendor: "test",
        fingerprints: FINGERPRINTS,
        exposes: DIM_CT_EXPOSES,
        from: DIM_CT_FROM,
        to: DIM_CT_TO,
        configure: Some(&DIM_CT_CONFIGURE),
        omit_optional_params: false,
        turns_off_at_brightness_1: false,
        support: Support::IDENTIFIED
            .union(Support::DECODED)
            .union(Support::CONTROLLED),
    };

    static COLOR_PROFILE: Profile = Profile {
        id: "test.color",
        vendor: "test",
        fingerprints: FINGERPRINTS,
        exposes: &[Expose::Color { property: COLOR }],
        from: &[COLOR_FRAME],
        to: &[COLOR_XY_TO, COLOR_HS_TO],
        configure: None,
        omit_optional_params: false,
        turns_off_at_brightness_1: false,
        support: Support::IDENTIFIED
            .union(Support::DECODED)
            .union(Support::CONTROLLED),
    };

    static ENHANCED_PROFILE: Profile = Profile {
        id: "test.color.enhanced",
        vendor: "test",
        fingerprints: FINGERPRINTS,
        exposes: &[Expose::Color { property: COLOR }],
        from: &[COLOR_FRAME],
        to: &[COLOR_ENHANCED_TO],
        configure: None,
        omit_optional_params: false,
        turns_off_at_brightness_1: false,
        support: Support::IDENTIFIED
            .union(Support::DECODED)
            .union(Support::CONTROLLED),
    };

    fn color_updates<'a>(profile: &Profile, payload: &'a [u8]) -> Vec<Update> {
        let frame = AttributeFrameView::parse(0x0300, 1, 0x0A, payload).unwrap();
        let mut decoded = crate::Decoded::default();
        crate::decode_frame(profile, &frame, &mut decoded);
        decoded.updates().copied().collect()
    }

    #[test]
    fn color_frame_assembles_xy_and_color_mode() {
        let payload = [
            0x03, 0x00, 0x21, 0x00, 0x7D, // currentX = 32000
            0x04, 0x00, 0x21, 0xC0, 0x5D, // currentY = 24000
            0x08, 0x00, 0x30, 0x01, // colorMode = xy
        ];
        let updates = color_updates(&COLOR_PROFILE, &payload);
        let color = updates.iter().find_map(|update| match update {
            Update::Property {
                property,
                value: Value::Color(color),
            } if *property == COLOR => Some(*color),
            _ => None,
        });
        let Some(ColorValue::Xy {
            x: Some(x),
            y: Some(y),
        }) = color
        else {
            panic!("expected xy color, got {updates:?}");
        };
        assert!((x - 32000.0 / 65535.0).abs() < 1e-5);
        assert!((y - 24000.0 / 65535.0).abs() < 1e-5);
        assert!(updates.iter().any(|update| matches!(
            update,
            Update::Property { property, value: Value::Enum(1) } if *property == COLOR_MODE
        )));
    }

    #[test]
    fn partial_color_frame_keeps_missing_coordinate_missing() {
        let updates = color_updates(&COLOR_PROFILE, &[0x03, 0x00, 0x21, 0x00, 0x7D]);
        assert!(updates.iter().any(|update| matches!(
            update,
            Update::Property {
                property: COLOR,
                value: Value::Color(ColorValue::Xy {
                    x: Some(_),
                    y: None
                }),
            }
        )));
    }

    #[test]
    fn hue_saturation_frame_decodes_as_hs() {
        let payload = [
            0x00, 0x00, 0x20, 0x7F, // currentHue = 127 -> 180
            0x01, 0x00, 0x20, 0x7F, // currentSaturation = 127 -> 50
            0x08, 0x00, 0x30, 0x00, // colorMode = hs
        ];
        let updates = color_updates(&COLOR_PROFILE, &payload);
        let color = updates.iter().find_map(|update| match update {
            Update::Property {
                property,
                value: Value::Color(color),
            } if *property == COLOR => Some(*color),
            _ => None,
        });
        let Some(ColorValue::Hs {
            hue: Some(hue),
            saturation: Some(saturation),
        }) = color
        else {
            panic!("expected hs color");
        };
        assert!((hue - 180.0).abs() < 0.01);
        assert!((saturation - 50.0).abs() < 0.01);
    }

    #[test]
    fn color_commands_match_zcl_payloads() {
        let mut out = [0u8; 12];
        let xy = Command::Property {
            property: COLOR,
            value: Value::Color(ColorValue::xy(0.5, 0.25)),
        };
        assert_eq!(encode(&COLOR_PROFILE, &xy, &mut out), Some(8));
        assert_eq!(&out[..8], &[0x00, 0x80, 0x00, 0x40, 0, 0, 0, 0]);

        let hs = Command::Property {
            property: COLOR,
            value: Value::Color(ColorValue::hs(180.0, 50.0)),
        };
        assert_eq!(encode(&COLOR_PROFILE, &hs, &mut out), Some(6));
        assert_eq!(&out[..6], &[127, 127, 0, 0, 0, 0]);

        assert_eq!(encode(&ENHANCED_PROFILE, &hs, &mut out), Some(7));
        assert_eq!(&out[..7], &[0x00, 0x80, 127, 0, 0, 0, 0]);
    }

    #[test]
    fn composition_identifies_and_decodes() {
        assert_eq!(PROFILE.id, "test.dimct");
        let event = RawZcl {
            device: [1; 8],
            endpoint: 1,
            cluster: 0x0008,
            command: 0x0A,
            attribute: Some(0x0000),
            payload: &[127],
        };
        let decoded = decode(&PROFILE, &event).unwrap();
        assert_eq!(
            decoded.updates().copied().collect::<Vec<_>>(),
            vec![Update::Property {
                property: BRIGHTNESS,
                value: Value::Int(127),
            }]
        );
    }

    #[test]
    fn level_and_color_temp_encode_dynamic_payload() {
        let mut out = [0u8; 8];
        let level = Command::Property {
            property: BRIGHTNESS,
            value: Value::Int(100),
        };
        assert_eq!(encode(&PROFILE, &level, &mut out), Some(5));
        assert_eq!(&out[..5], &[100, 0, 0, 0, 0]);

        let temp = Command::Property {
            property: COLOR_TEMP,
            value: Value::Int(350),
        };
        assert_eq!(encode(&PROFILE, &temp, &mut out), Some(6));
        assert_eq!(&out[..6], &[0x5E, 0x01, 0, 0, 0, 0]);
    }
}
