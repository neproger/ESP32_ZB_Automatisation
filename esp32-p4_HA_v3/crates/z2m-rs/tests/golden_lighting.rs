//! Generated lighting profiles must remain compositions with explicit limits.

use z2m_rs::capabilities::{DIM_CT_CONFIGURE, DIM_CT_READ};
use z2m_rs::{
    decode_frame, encode, identify, properties::BRIGHTNESS, properties::COLOR,
    properties::COLOR_TEMP, AttributeFrameView, ColorValue, Command, Decoded, Expose, Interview,
    Manufacturer, RawZcl, Support, Update, Value, LIGHTING,
};

#[test]
fn generated_lighting_profiles_are_explicitly_partial() {
    assert!(LIGHTING.len() > 500, "lighting catalog unexpectedly small");
    let mut configured = 0;
    let mut unconfigured = 0;
    for profile in LIGHTING {
        assert!(
            !profile.fingerprints.is_empty(),
            "{} has no fingerprint",
            profile.id
        );
        assert!(!profile.exposes.is_empty(), "{} has no exposes", profile.id);
        assert!(
            !profile.from.is_empty(),
            "{} has no from converters",
            profile.id
        );
        assert!(!profile.to.is_empty(), "{} has no commands", profile.id);
        assert!(profile.support.contains(Support::DECODED));
        assert!(profile.support.contains(Support::CONTROLLED));
        assert!(
            profile.support.contains(Support::PARTIAL),
            "{} claims full Z2M support",
            profile.id
        );
        if profile.configure.is_some() {
            configured += 1;
        } else {
            unconfigured += 1;
        }
    }
    assert!(configured > 0);
    assert!(
        unconfigured > 0,
        "non-reporting dim lights should have no configure plan"
    );
}

fn ct_range(profile: &z2m_rs::Profile) -> Option<(f32, f32)> {
    profile.exposes.iter().find_map(|expose| match expose {
        Expose::Numeric {
            property, min, max, ..
        } if *property == COLOR_TEMP => Some((*min, *max)),
        _ => None,
    })
}

#[test]
fn generated_profiles_carry_original_color_temp_ranges() {
    let mut ranges = std::collections::BTreeSet::new();
    for profile in LIGHTING {
        if let Some(range) = ct_range(profile) {
            ranges.insert((range.0 as u32, range.1 as u32));
        }
    }
    assert!(
        ranges.len() > 5,
        "color temp ranges look defaulted: {ranges:?}"
    );

    let ikea = LIGHTING
        .iter()
        .copied()
        .find(|profile| profile.id == "zhc.ikea.0")
        .expect("ikea profile");
    assert_eq!(ct_range(ikea), Some((250.0, 454.0)));
    assert!(ikea.configure.is_some());
    assert!(std::ptr::eq(ikea.configure.unwrap(), &DIM_CT_READ));
    assert!(LIGHTING.iter().any(|profile| {
        profile
            .configure
            .is_some_and(|plan| std::ptr::eq(plan, &DIM_CT_CONFIGURE))
    }));
}

#[test]
fn generated_light_identifies_and_encodes_brightness() {
    let profile = LIGHTING
        .iter()
        .copied()
        .find(|profile| profile.exposes.len() == 3)
        .expect("dim+color-temp profile");
    let fingerprint = &profile.fingerprints[0];
    let manufacturer = match &fingerprint.manufacturer {
        Manufacturer::Exact(name) => Some(*name),
        Manufacturer::Prefix(_) | Manufacturer::Any => None,
    };
    let found = identify(&Interview {
        manufacturer,
        model: fingerprint.model,
    })
    .expect("generated light must identify");
    assert!(
        found.id.starts_with("zhc."),
        "unexpected profile {}",
        found.id
    );

    let level = RawZcl {
        device: [1; 8],
        endpoint: 1,
        cluster: 0x0008,
        command: 0x0A,
        attribute: Some(0x0000),
        payload: &[127],
    };
    let decoded = z2m_rs::decode(found, &level).expect("level report decodes");
    assert!(decoded.updates().any(|update| matches!(
        update,
        Update::Property { property, .. } if *property == BRIGHTNESS
    )));

    let mut out = [0u8; 8];
    let command = Command::Property {
        property: BRIGHTNESS,
        value: Value::Int(100),
    };
    assert_eq!(encode(found, &command, &mut out), Some(5));
    assert_eq!(&out[..5], &[100, 0, 0, 0, 0]);
}
#[test]
fn generated_color_profiles_decode_and_encode() {
    let profile = LIGHTING
        .iter()
        .copied()
        .find(|profile| {
            profile
                .exposes
                .iter()
                .any(|expose| matches!(expose, Expose::Color { .. }))
        })
        .expect("generated color profile");

    let payload = [
        0x03, 0x00, 0x21, 0x00, 0x7D, // currentX = 32000
        0x04, 0x00, 0x21, 0xC0, 0x5D, // currentY = 24000
        0x08, 0x00, 0x30, 0x01, // colorMode = xy
    ];
    let frame = AttributeFrameView::parse(0x0300, 1, 0x0A, &payload).unwrap();
    let mut decoded = Decoded::default();
    decode_frame(profile, &frame, &mut decoded);
    assert!(decoded.updates().any(|update| matches!(
        update,
        Update::Property {
            property,
            value: Value::Color(ColorValue::Xy { x: Some(_), y: Some(_) }),
        } if *property == COLOR
    )));

    let mut out = [0u8; 12];
    let xy = Command::Property {
        property: COLOR,
        value: Value::Color(ColorValue::xy(0.5, 0.25)),
    };
    let hs = Command::Property {
        property: COLOR,
        value: Value::Color(ColorValue::hs(180.0, 50.0)),
    };
    assert!(encode(profile, &xy, &mut out).is_some() || encode(profile, &hs, &mut out).is_some());
}
