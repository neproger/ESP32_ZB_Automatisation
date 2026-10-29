//! Profile adapter: capabilities/commands derived from `z2m-rs` profiles.
//!
//! The standard ZCL profile is just another profile; endpoint discovery and
//! confirmed attributes select what a concrete endpoint actually exposes.

use ha_domain::canonical::{
    CapabilitiesRecord, Capability, PropertyId, PROPERTY_BATTERY_PERCENT, PROPERTY_BATTERY_VOLTAGE,
    PROPERTY_BRIGHTNESS, PROPERTY_COLOR, PROPERTY_COLOR_TEMPERATURE, PROPERTY_HUMIDITY,
    PROPERTY_ILLUMINANCE, PROPERTY_OCCUPANCY, PROPERTY_POWER, PROPERTY_TEMPERATURE,
};
use z2m_rs::capabilities::STANDARD_PROFILE;
use z2m_rs::{properties, Profile, Update};

use crate::descriptor::EndpointDescriptor;
use crate::projection::profile_capabilities;
use crate::zcl::{self, Confirmed};

pub fn capabilities_for(
    profile: &Profile,
    endpoint: Option<&EndpointDescriptor>,
    confirmed: &Confirmed,
) -> CapabilitiesRecord {
    let all = profile_capabilities(profile);
    let mut out = CapabilitiesRecord::default();
    for capability in &all.properties[..usize::from(all.property_count)] {
        if let Some(endpoint) = endpoint {
            let Some((cluster, attributes)) = zcl::property_attributes(capability.property) else {
                continue;
            };
            if !endpoint.has_server(cluster) {
                continue;
            }
            // OnOff OnOff is mandatory for an OnOff server. Requiring a read
            // before the first automation command makes startup/button presses
            // fail with InvalidArg until the configure walk reaches the device.
            let onoff_is_mandatory = capability.property == PROPERTY_POWER && cluster == zcl::ONOFF;
            if !onoff_is_mandatory
                && !attributes
                    .iter()
                    .any(|attribute| confirmed.is_confirmed(endpoint.endpoint, cluster, *attribute))
            {
                continue;
            }
        }
        out.push(*capability);
    }
    for event in &all.events[..usize::from(all.event_count)] {
        out.push_event(*event);
    }
    out
}

pub fn capabilities_confirmed(
    endpoint: &EndpointDescriptor,
    confirmed: &Confirmed,
) -> CapabilitiesRecord {
    capabilities_for(&STANDARD_PROFILE, Some(endpoint), confirmed)
}

pub fn command_allowed(
    profile: &Profile,
    endpoint: Option<&EndpointDescriptor>,
    confirmed: &Confirmed,
    property: PropertyId,
    action: u32,
) -> bool {
    let caps = capabilities_for(profile, endpoint, confirmed);
    caps.properties[..usize::from(caps.property_count)]
        .iter()
        .any(|capability: &Capability| {
            capability.property == property
                && capability.actions[..usize::from(capability.action_count)].contains(&action)
        })
}

pub fn capability_allows(
    endpoint: &EndpointDescriptor,
    confirmed: &Confirmed,
    property: PropertyId,
    action: u32,
) -> bool {
    command_allowed(
        &STANDARD_PROFILE,
        Some(endpoint),
        confirmed,
        property,
        action,
    )
}

pub fn update_property(update: &Update) -> Option<PropertyId> {
    let Update::Property { property, .. } = update else {
        return None;
    };
    Some(match *property {
        properties::STATE => PROPERTY_POWER,
        properties::BRIGHTNESS => PROPERTY_BRIGHTNESS,
        properties::COLOR_TEMP => PROPERTY_COLOR_TEMPERATURE,
        properties::COLOR => PROPERTY_COLOR,
        properties::BATTERY => PROPERTY_BATTERY_PERCENT,
        properties::VOLTAGE => PROPERTY_BATTERY_VOLTAGE,
        properties::TEMPERATURE => PROPERTY_TEMPERATURE,
        properties::HUMIDITY => PROPERTY_HUMIDITY,
        properties::OCCUPANCY => PROPERTY_OCCUPANCY,
        properties::ILLUMINANCE => PROPERTY_ILLUMINANCE,
        _ => return None,
    })
}

fn update_is_discovered_for_profile(
    _profile: &Profile,
    endpoint: Option<&EndpointDescriptor>,
    update: &Update,
) -> bool {
    let Some(property) = update_property(update) else {
        return false;
    };
    let Some(endpoint) = endpoint else {
        return true;
    };
    let Some((cluster, _)) = zcl::property_attributes(property) else {
        return false;
    };
    endpoint.has_server(cluster)
}

pub fn update_is_discovered(endpoint: &EndpointDescriptor, update: &Update) -> bool {
    update_is_discovered_for_profile(&STANDARD_PROFILE, Some(endpoint), update)
}

pub fn update_capability(_endpoint: &EndpointDescriptor, update: &Update) -> Option<PropertyId> {
    update_property(update)
}

pub fn decode_standard(
    endpoint: &EndpointDescriptor,
    event: &z2m_rs::RawZcl<'_>,
) -> Option<z2m_rs::Decoded> {
    if !endpoint.has_server(event.cluster) {
        return None;
    }
    z2m_rs::decode(&STANDARD_PROFILE, event)
}
