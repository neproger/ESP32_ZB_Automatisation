include!("golden/matcher_complex_cases.rs");

use z2m_rs::{match_definition, DeviceIdentity};

#[test]
fn matcher_matches_zhc_for_complex_fingerprints() {
    let mut checked = 0;
    let mut failures = Vec::new();
    for case in CASES {
        let device = DeviceIdentity {
            model: case.model,
            manufacturer_name: case.manufacturer_name,
            manufacturer_id: case.manufacturer_id,
            type_: case.type_,
            power_source: case.power_source,
            software_build_id: case.software_build_id,
            date_code: case.date_code,
            application_version: case.application_version,
            hardware_version: case.hardware_version,
            zcl_version: case.zcl_version,
            ieee: case.ieee,
            endpoints: case.endpoints,
        };
        match match_definition(&device) {
            Some(definition) if definition.id == case.expected => checked += 1,
            Some(definition) => failures.push(format!(
                "{} {}/{:?}: expected {}, got {}",
                case.category, case.model, case.manufacturer_name, case.expected, definition.id
            )),
            None => failures.push(format!(
                "{} {}/{:?}: expected {}, got none",
                case.category, case.model, case.manufacturer_name, case.expected
            )),
        }
    }
    assert!(checked > 300, "only {checked} complex cases checked");
    assert!(
        failures.is_empty(),
        "{} of {} complex cases differ from ZHC:\n{}",
        failures.len(),
        checked + failures.len(),
        failures.iter().take(15).cloned().collect::<Vec<_>>().join("\n")
    );
}