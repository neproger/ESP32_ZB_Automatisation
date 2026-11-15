use super::*;

pub(super) fn post_property(
    entity: EntityId,
    property: PropertyId,
    action: u32,
    value: CommandValue,
) -> Result<(), ha_domain::Error> {
    let command = CanonicalCommand {
        entity,
        property,
        action,
        value,
    };
    domain()?.post(
        COMMAND_PROPERTY,
        &command.encode(),
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}

pub(super) fn post_settings(brightness: u8) -> Result<(), ha_domain::Error> {
    domain()?.post(
        COMMAND_SETTINGS,
        &[brightness],
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}

pub(super) fn post_automation(automation: u64, enabled: bool) -> Result<(), ha_domain::Error> {
    let mut args = [0u8; 9];
    args[..8].copy_from_slice(&automation.to_le_bytes());
    args[8] = u8::from(enabled);
    domain()?.post(
        COMMAND_AUTOMATION_ENABLE,
        &args,
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}
pub(super) fn post_wifi_connect(ssid: &str, password: &str) -> Result<(), ha_domain::Error> {
    domain()?.post(
        COMMAND_WIFI_CONNECT,
        &ha_domain::canonical::encode_wifi_connect(ssid, password),
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}

pub fn request_wifi_scan() -> Result<(), ha_domain::Error> {
    domain()?.post(
        COMMAND_WIFI_SCAN,
        &[],
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}

