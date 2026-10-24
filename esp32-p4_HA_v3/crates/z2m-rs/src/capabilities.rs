//! Shared Zigbee capabilities. Profiles compose these fragments; they do not
//! describe device families or models.

use crate::properties::{BRIGHTNESS, COLOR_TEMP, STATE};
use crate::{
    AttributeRule, AttrType, Command, CommandCodec, CommandMap, ConfigureEndpoint, ConfigurePlan,
    ConfigureStep, Converter, Expose, Scale, Unit, Value,
};

pub const ONOFF_ATTR: Converter = Converter::Attribute(AttributeRule {
    cluster: 0x0006,
    attribute: 0x0000,
    kind: AttrType::Bool,
    property: STATE,
    scale: Scale::NONE,
    skip: None,
});

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

pub static DIM_CT_FROM: &[Converter] = &[ONOFF_ATTR, LEVEL_ATTR, COLORTEMP_ATTR];
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::properties::{BRIGHTNESS, COLOR_TEMP};
    use crate::{
        decode, encode, Command, Fingerprint, Manufacturer, Profile, RawZcl, Support, Update,
        Value,
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
        support: Support::IDENTIFIED
            .union(Support::DECODED)
            .union(Support::CONTROLLED),
    };

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
        assert_eq!(encode(&PROFILE, &level, &mut out), Some(3));
        assert_eq!(&out[..3], &[100, 0, 0]);

        let temp = Command::Property {
            property: COLOR_TEMP,
            value: Value::Int(350),
        };
        assert_eq!(encode(&PROFILE, &temp, &mut out), Some(4));
        assert_eq!(&out[..4], &[0x5E, 0x01, 0, 0]);
    }
}
