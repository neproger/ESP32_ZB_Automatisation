// Hand-ported Tuya on/off fragment; the spike generator does not cover this
// definition yet (see docs/zigbee/reports/Z2M_GENERATOR_AUDIT.md).
use crate::properties::STATE;
use crate::{
    AttributeRule, AttrType, Command, CommandCodec, CommandMap, ConfigureEndpoint, ConfigurePlan,
    ConfigureStep, Converter, Expose, Fingerprint, Manufacturer, Profile, Scale, Support, Value,
    TUYA_MAGIC_ATTRIBUTES,
};

// Fragment: type:tuya.tuyaOnOff core (standard genOnOff state + on/off commands).
// Electrical measurements, options (indicatorMode, onOffCountdown, childLock) and
// metering divisors are NOT generated: support stays PARTIAL.
pub static TUYA_ON_OFF_STATE_EXPOSES: &[Expose] = &[Expose::Binary { property: STATE }];

pub static TUYA_ON_OFF_STATE_FROM: &[Converter] = &[Converter::Attribute(AttributeRule {
    cluster: 0x0006,
    attribute: 0x0000,
    kind: AttrType::Bool,
    property: STATE,
    scale: Scale::NONE,
    skip: None,
})];

pub static TUYA_ON_OFF_STATE_TO: &[CommandMap] = &[
    CommandMap {
        command: Command::Property {
            property: STATE,
            value: Value::Bool(true),
        },
        cluster: 0x0006,
        zcl_command: 0x01,
        args: &[],
        codec: CommandCodec::None,
    },
    CommandMap {
        command: Command::Property {
            property: STATE,
            value: Value::Bool(false),
        },
        cluster: 0x0006,
        zcl_command: 0x00,
        args: &[],
        codec: CommandCodec::None,
    },
];

/// ZHC TS011F plug: magic packet, genOnOff bind and on/off reporting. Electrical
/// reporting is intentionally not part of this first control profile.
pub static TS011F_CONFIGURE: ConfigurePlan = ConfigurePlan {
    steps: &[
        ConfigureStep::Read {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0000,
            attributes: TUYA_MAGIC_ATTRIBUTES,
        },
        ConfigureStep::Bind {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
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
    ],
    timeout_ms: 30_000,
    retries: 3,
    version: 1,
};

pub static TS011F: Profile = Profile {
    id: "tuya.ts011f",
    vendor: "Tuya",
    fingerprints: &[
        Fingerprint {
            manufacturer: Manufacturer::Prefix("_TZ3000_"),
            model: "TS011F",
        },
        Fingerprint {
            manufacturer: Manufacturer::Prefix("_TZ3008_"),
            model: "TS011F",
        },
        Fingerprint {
            manufacturer: Manufacturer::Prefix("_TZ3210_"),
            model: "TS011F",
        },
    ],
    exposes: TUYA_ON_OFF_STATE_EXPOSES,
    from: TUYA_ON_OFF_STATE_FROM,
    to: TUYA_ON_OFF_STATE_TO,
    configure: Some(&TS011F_CONFIGURE),
    // Core state/commands/configure work; electrical measurements and options are
    // not generated yet.
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED
        .union(Support::DECODED)
        .union(Support::CONTROLLED)
        .union(Support::PARTIAL),
};
