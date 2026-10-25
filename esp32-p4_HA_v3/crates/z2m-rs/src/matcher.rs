use crate::Profile;

pub struct EndpointRule {
    pub id: Option<u8>,
    pub device_id: Option<u16>,
    pub profile_id: Option<u16>,
    pub input: &'static [u16],
    pub output: &'static [u16],
}

pub struct IeeeRule {
    pub prefix: &'static str,
    pub wildcards: u8,
}

pub struct FingerprintRule {
    pub manufacturer_name: Option<&'static str>,
    pub model_id: Option<&'static str>,
    pub manufacturer_id: Option<u16>,
    pub type_: Option<&'static str>,
    pub power_source: Option<&'static str>,
    pub software_build_id: Option<&'static str>,
    pub date_code: Option<&'static str>,
    pub application_version: Option<u16>,
    pub hardware_version: Option<u16>,
    pub zcl_version: Option<u16>,
    pub priority: i16,
    pub ieee: Option<IeeeRule>,
    pub endpoints: Option<&'static [EndpointRule]>,
}

pub struct WhiteLabel {
    pub model: &'static str,
    pub vendor: &'static str,
    pub description: Option<&'static str>,
}

pub struct Definition {
    pub id: &'static str,
    pub source: &'static str,
    pub model: &'static str,
    pub vendor: &'static str,
    pub profile: Option<&'static Profile>,
    pub zigbee_models: &'static [&'static str],
    pub fingerprints: &'static [FingerprintRule],
    pub white_labels: &'static [WhiteLabel],
}

pub struct EndpointIdentity<'a> {
    pub id: u8,
    pub device_id: Option<u16>,
    pub profile_id: Option<u16>,
    pub input: &'a [u16],
    pub output: &'a [u16],
}

pub struct DeviceIdentity<'a> {
    pub model: &'a str,
    pub manufacturer_name: Option<&'a str>,
    pub manufacturer_id: Option<u16>,
    pub type_: Option<&'a str>,
    pub power_source: Option<&'a str>,
    pub software_build_id: Option<&'a str>,
    pub date_code: Option<&'a str>,
    pub application_version: Option<u16>,
    pub hardware_version: Option<u16>,
    pub zcl_version: Option<u16>,
    pub ieee: Option<&'a str>,
    pub endpoints: &'a [EndpointIdentity<'a>],
}

fn normalized(value: &str) -> &str {
    let end = value.find('\0').unwrap_or(value.len());
    value[..end].trim()
}

fn candidates(model: &str) -> &'static [u16] {
    let index = crate::generated::MODEL_KEYS
        .iter()
        .position(|key| key.eq_ignore_ascii_case(model))
        .or_else(|| {
            let normalized = normalized(model);
            crate::generated::MODEL_KEYS
                .iter()
                .position(|key| key.eq_ignore_ascii_case(normalized))
        });
    match index {
        Some(index) => crate::generated::MODEL_CANDIDATES[index],
        None => &[],
    }
}

fn array_equals(left: &[u16], right: &[u16]) -> bool {
    left.len() == right.len() && left.iter().all(|value| right.contains(value))
}

fn optional_str(rule: Option<&str>, value: Option<&str>) -> bool {
    rule.is_none() || rule == value
}

fn endpoint_matches(rule: &EndpointRule, endpoint: &EndpointIdentity<'_>) -> bool {
    (rule.device_id.is_none() || rule.device_id == endpoint.device_id)
        && (rule.profile_id.is_none() || rule.profile_id == endpoint.profile_id)
        && array_equals(rule.input, endpoint.input)
        && array_equals(rule.output, endpoint.output)
}

fn fingerprint_matches(rule: &FingerprintRule, device: &DeviceIdentity<'_>) -> bool {
    optional_str(rule.manufacturer_name, device.manufacturer_name)
        && optional_str(rule.model_id, Some(device.model))
        && (rule.manufacturer_id.is_none() || rule.manufacturer_id == device.manufacturer_id)
        && optional_str(rule.type_, device.type_)
        && optional_str(rule.power_source, device.power_source)
        && optional_str(rule.software_build_id, device.software_build_id)
        && optional_str(rule.date_code, device.date_code)
        && (rule.application_version.is_none() || rule.application_version == device.application_version)
        && (rule.hardware_version.is_none() || rule.hardware_version == device.hardware_version)
        && (rule.zcl_version.is_none() || rule.zcl_version == device.zcl_version)
        && match &rule.ieee {
            None => true,
            Some(ieee) => device.ieee.is_some_and(|value| {
                value.len() == ieee.prefix.len() + usize::from(ieee.wildcards)
                    && value.starts_with(ieee.prefix)
            }),
        }
        && match rule.endpoints {
            None => true,
            Some(rules) => {
                let ids: &[u8] = &device.endpoints.iter().map(|endpoint| endpoint.id).collect::<Vec<_>>();
                let expected: &[u8] = &rules.iter().filter_map(|rule| rule.id).collect::<Vec<_>>();
                ids.len() == expected.len()
                    && ids.iter().all(|id| expected.contains(id))
                    && rules.iter().all(|rule| {
                        device
                            .endpoints
                            .iter()
                            .find(|endpoint| Some(endpoint.id) == rule.id)
                            .is_some_and(|endpoint| endpoint_matches(rule, endpoint))
                    })
            }
        }
}

/// Mirrors ZHC `findDefinition`: index candidates, single-candidate shortcut,
/// fingerprint priority, then zigbeeModel fallback. No diagnostic statuses.
pub fn match_definition(device: &DeviceIdentity<'_>) -> Option<&'static Definition> {
    let candidates = candidates(device.model);
    if candidates.len() == 1 && !candidate(candidates[0]).zigbee_models.is_empty() {
        return Some(candidate(candidates[0]));
    }
    let mut selected: Option<(&'static Definition, i16)> = None;
    for index in candidates {
        let definition = candidate(*index);
        for fingerprint in definition.fingerprints {
            let matched = fingerprint_matches(fingerprint, device);
            if matched
                && selected.map_or(true, |(_, priority)| fingerprint.priority > priority)
            {
                selected = Some((definition, fingerprint.priority));
            }
        }
    }
    if let Some((definition, _)) = selected {
        return Some(definition);
    }
    let model = normalized(device.model);
    for index in candidates {
        let definition = candidate(*index);
        if definition
            .zigbee_models
            .iter()
            .any(|candidate| *candidate == device.model || *candidate == model)
        {
            return Some(definition);
        }
    }
    None
}

fn candidate(index: u16) -> &'static Definition {
    &crate::generated::DEFINITIONS[usize::from(index)]
}