//! Generated lighting profiles must remain compositions, not silent fallbacks.

use z2m_rs::{
    encode, identify, properties::BRIGHTNESS, Command, Interview, Manufacturer, RawZcl, Support,
    Update, Value, LIGHTING,
};

#[test]
fn generated_lighting_profiles_are_explicitly_partial() {
    assert!(LIGHTING.len() > 500, "lighting catalog unexpectedly small");
    for profile in LIGHTING {
        assert!(!profile.fingerprints.is_empty(), "{} has no fingerprint", profile.id);
        assert!(!profile.exposes.is_empty(), "{} has no exposes", profile.id);
        assert!(!profile.from.is_empty(), "{} has no from converters", profile.id);
        assert!(!profile.to.is_empty(), "{} has no commands", profile.id);
        assert!(profile.configure.is_some(), "{} has no configure plan", profile.id);
        assert!(profile.support.contains(Support::DECODED));
        assert!(profile.support.contains(Support::CONTROLLED));
        assert!(profile.support.contains(Support::PARTIAL), "{} claims full Z2M support", profile.id);
    }
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
    assert!(found.id.starts_with("zhc."), "unexpected profile {}", found.id);

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
    assert_eq!(encode(found, &command, &mut out), Some(3));
    assert_eq!(&out[..3], &[100, 0, 0]);
}
