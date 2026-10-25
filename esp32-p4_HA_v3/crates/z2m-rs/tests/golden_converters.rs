//! Differential golden: commands produced by the original ZHC toZigbee
//! converters (tools/z2m_spike/converters_golden.cjs) vs z2m-rs encoding.

use z2m_rs::capabilities::*;
use z2m_rs::properties::{BRIGHTNESS, COLOR, COLOR_TEMP, STATE};
use z2m_rs::{
    encode, resolve_command, ColorValue, Command, Profile, Support, Value,
};

static BASE: Profile = Profile {
    id: "golden.converters",
    vendor: "golden",
    fingerprints: &[],
    exposes: &[],
    from: &[],
    to: &[ONOFF_ON, ONOFF_OFF, LEVEL_TO, COLORTEMP_TO, COLOR_XY_TO, COLOR_HS_TO],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

static OMIT: Profile = Profile {
    id: "golden.converters.omit",
    vendor: "golden",
    fingerprints: &[],
    exposes: &[],
    from: &[],
    to: &[ONOFF_ON, ONOFF_OFF, LEVEL_TO, COLORTEMP_TO, COLOR_XY_TO, COLOR_HS_TO],
    configure: None,
    omit_optional_params: true,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

static TURNS: Profile = Profile {
    id: "golden.converters.turns1",
    vendor: "golden",
    fingerprints: &[],
    exposes: &[],
    from: &[],
    to: &[ONOFF_ON, ONOFF_OFF, LEVEL_TO],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: true,
    support: Support::IDENTIFIED,
};

static ENHANCED: Profile = Profile {
    id: "golden.converters.enhanced",
    vendor: "golden",
    fingerprints: &[],
    exposes: &[],
    from: &[],
    to: &[
        ONOFF_ON,
        ONOFF_OFF,
        LEVEL_TO,
        COLORTEMP_TO,
        COLOR_XY_TO,
        COLOR_ENHANCED_TO,
    ],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

fn hex(bytes: &str) -> Vec<u8> {
    bytes
        .as_bytes()
        .chunks(2)
        .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
        .collect()
}

fn cluster(name: &str) -> u16 {
    match name {
        "genOnOff" => 0x0006,
        "genLevelCtrl" => 0x0008,
        "lightingColorCtrl" => 0x0300,
        other => panic!("unknown cluster {other}"),
    }
}

#[test]
fn zcl_payloads_match_original_converters() {
    for line in include_str!("golden/converters.tsv").lines() {
        if line.trim().is_empty() {
            continue;
        }
        let mut fields = line.split('\t');
        let case = fields.next().unwrap();
        let cluster_name = fields.next().unwrap();
        let _command_name = fields.next().unwrap();
        let zcl: u8 = u8::from_str_radix(fields.next().unwrap().trim_start_matches("0x"), 16).unwrap();
        let expected = hex(fields.next().unwrap());

        let (profile, command) = match case {
            "state_on" => (
                &BASE,
                Command::Property {
                    property: STATE,
                    value: Value::Bool(true),
                },
            ),
            "state_off" => (
                &BASE,
                Command::Property {
                    property: STATE,
                    value: Value::Bool(false),
                },
            ),
            "brightness" => (
                &BASE,
                Command::Property {
                    property: BRIGHTNESS,
                    value: Value::Int(100),
                },
            ),
            "color_temp" => (
                &BASE,
                Command::Property {
                    property: COLOR_TEMP,
                    value: Value::Int(350),
                },
            ),
            "color_xy" => (
                &BASE,
                Command::Property {
                    property: COLOR,
                    value: Value::Color(ColorValue::xy(0.5, 0.25)),
                },
            ),
            "color_hs" => (
                &BASE,
                Command::Property {
                    property: COLOR,
                    value: Value::Color(ColorValue::hs(180.0, 50.0)),
                },
            ),
            "brightness_omit" => (
                &OMIT,
                Command::Property {
                    property: BRIGHTNESS,
                    value: Value::Int(100),
                },
            ),
            "color_temp_omit" => (
                &OMIT,
                Command::Property {
                    property: COLOR_TEMP,
                    value: Value::Int(350),
                },
            ),
            "color_xy_omit" => (
                &OMIT,
                Command::Property {
                    property: COLOR,
                    value: Value::Color(ColorValue::xy(0.5, 0.25)),
                },
            ),
            "brightness_one" => (
                &TURNS,
                Command::Property {
                    property: BRIGHTNESS,
                    value: Value::Int(1),
                },
            ),
            "color_enhanced" => (
                &ENHANCED,
                Command::Property {
                    property: COLOR,
                    value: Value::Color(ColorValue::hs(180.0, 50.0)),
                },
            ),
            other => panic!("unknown golden case {other}"),
        };

        assert_eq!(
            resolve_command(profile, &command),
            Some((cluster(cluster_name), zcl)),
            "{case}"
        );
        let mut out = [0u8; 16];
        let len = encode(profile, &command, &mut out).expect("encoded");
        assert_eq!(&out[..len], expected.as_slice(), "{case}");
    }
}
