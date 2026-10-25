use z2m_rs::{identify, Interview};

#[test]
fn identify_matches_zhc_selection() {
    let data = include_str!("golden/identify.tsv");
    let mut checked = 0;
    let mut failures = Vec::new();
    for line in data.lines().skip(1) {
        let mut fields = line.splitn(3, '\t');
        let model = fields.next().expect("model").replace("\\0", "\0");
        let manufacturer = fields.next().expect("manufacturer").replace("\\0", "\0");
        let expected = fields.next().expect("profile");
        let interview = Interview {
            manufacturer: (!manufacturer.is_empty()).then_some(manufacturer.as_str()),
            model: &model,
        };
        match identify(&interview) {
            Some(profile) if profile.id == expected => checked += 1,
            Some(profile) => failures.push(format!(
                "{model:?}/{manufacturer:?}: expected {expected}, got {}",
                profile.id
            )),
            None => failures.push(format!(
                "{model:?}/{manufacturer:?}: expected {expected}, got none"
            )),
        }
    }
    assert!(checked > 3000, "only {checked} cases checked");
    assert!(
        failures.is_empty(),
        "{} of {} identify cases differ from ZHC:\n{}",
        failures.len(),
        checked + failures.len(),
        failures
            .iter()
            .take(12)
            .cloned()
            .collect::<Vec<_>>()
            .join("\n")
    );
}
