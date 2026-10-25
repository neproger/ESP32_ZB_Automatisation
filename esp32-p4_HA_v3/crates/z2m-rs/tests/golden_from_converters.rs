//! Differential golden: original ZHC fromZigbee output vs z2m-rs decoding.

use std::collections::BTreeMap;

use z2m_rs::capabilities::{COLOR_FRAME, LEVEL_ATTR, ONOFF_ATTR};
use z2m_rs::properties::{BRIGHTNESS, COLOR, COLOR_MODE, COLOR_TEMP, STATE};
use z2m_rs::{
    decode_frame, AttributeFrameView, ColorValue, Decoded, Profile, Support, Update, Value,
};

static PROFILE: Profile = Profile {
    id: "golden.from",
    vendor: "golden",
    fingerprints: &[],
    exposes: &[],
    from: &[ONOFF_ATTR, LEVEL_ATTR, COLOR_FRAME],
    to: &[],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

#[derive(Clone, Debug, PartialEq)]
enum Normalized {
    Text(String),
    Number(f32),
}

fn attrs(fields: &str) -> Vec<u8> {
    let mut out = Vec::new();
    if fields.is_empty() {
        return out;
    }
    for field in fields.split(',') {
        let mut parts = field.split(':');
        let id: u16 = parts.next().unwrap().parse().unwrap();
        let kind: u8 = parts.next().unwrap().parse().unwrap();
        let value: u32 = parts.next().unwrap().parse().unwrap();
        out.extend_from_slice(&id.to_le_bytes());
        out.push(kind);
        match kind {
            0x21 => out.extend_from_slice(&(value as u16).to_le_bytes()),
            _ => out.push(value as u8),
        }
    }
    out
}

fn normalized(decoded: &Decoded) -> BTreeMap<String, Normalized> {
    let mut out = BTreeMap::new();
    for update in decoded.updates() {
        let Update::Property { property, value } = update else {
            continue;
        };
        if *property == STATE {
            if let Value::Bool(on) = value {
                out.insert(
                    "state".into(),
                    Normalized::Text(if *on { "ON" } else { "OFF" }.into()),
                );
            }
        } else if *property == BRIGHTNESS {
            if let Value::Int(v) = value {
                out.insert("brightness".into(), Normalized::Number(*v as f32));
            }
        } else if *property == COLOR_TEMP {
            if let Value::Uint(v) = value {
                out.insert("color_temp".into(), Normalized::Number(*v as f32));
            }
        } else if *property == COLOR_MODE {
            if let Value::Enum(v) = value {
                let mode = match v {
                    0 => "hs",
                    1 => "xy",
                    2 => "color_temp",
                    _ => "unknown",
                };
                out.insert("color_mode".into(), Normalized::Text(mode.into()));
            }
        } else if *property == COLOR {
            if let Value::Color(color) = value {
                match color {
                    ColorValue::Xy { x, y } => {
                        if let Some(v) = x {
                            out.insert("x".into(), Normalized::Number(*v));
                        }
                        if let Some(v) = y {
                            out.insert("y".into(), Normalized::Number(*v));
                        }
                    }
                    ColorValue::Hs { hue, saturation } => {
                        if let Some(v) = hue {
                            out.insert("hue".into(), Normalized::Number(*v));
                        }
                        if let Some(v) = saturation {
                            out.insert("saturation".into(), Normalized::Number(*v));
                        }
                    }
                }
            }
        }
    }
    out
}

fn expected(fields: &str) -> BTreeMap<String, Normalized> {
    let mut out = BTreeMap::new();
    if fields.is_empty() {
        return out;
    }
    for field in fields.split(',') {
        let (key, value) = field.split_once('=').unwrap();
        let normalized = match key {
            "state" | "color_mode" => Normalized::Text(value.into()),
            _ => Normalized::Number(value.parse().unwrap()),
        };
        out.insert(key.into(), normalized);
    }
    out
}

#[test]
fn from_converters_match_normalized_values() {
    for line in include_str!("golden/from_converters.tsv").lines() {
        if line.trim().is_empty() {
            continue;
        }
        let mut fields = line.split('\t');
        let case = fields.next().unwrap();
        let cluster =
            u16::from_str_radix(fields.next().unwrap().trim_start_matches("0x"), 16).unwrap();
        let attrs = attrs(fields.next().unwrap());
        let expected = expected(fields.next().unwrap());

        let frame = AttributeFrameView::parse(cluster, 1, 0x0A, &attrs).unwrap();
        let mut decoded = Decoded::default();
        decode_frame(&PROFILE, &frame, &mut decoded);
        let actual = normalized(&decoded);

        assert_eq!(
            actual.len(),
            expected.len(),
            "{case}: {actual:?} vs {expected:?}"
        );
        for (key, want) in &expected {
            match (actual.get(key), want) {
                (Some(Normalized::Text(got)), Normalized::Text(want)) => {
                    assert_eq!(got, want, "{case}:{key}")
                }
                (Some(Normalized::Number(got)), Normalized::Number(want)) => {
                    assert!(
                        (got - want).abs() < 0.001,
                        "{case}:{key} => {got} vs {want}"
                    )
                }
                (got, _) => panic!("{case}:{key} missing/mismatched: {got:?}"),
            }
        }
    }
}
