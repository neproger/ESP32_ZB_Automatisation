pub type EntityId = u64;
pub type DeviceId = u64;
pub type SourceId = u32;
pub type IssuerId = u32;
pub type PropertyId = u32;
pub type ActionId = u32;
pub type EventId = u32;

pub const ENTITY_ID_NONE: EntityId = 0;
pub const DEVICE_ID_NONE: DeviceId = 0;
/// V2 `HA_SYSTEM_DEVICE_ID`: key of the single Location record.
pub const SYSTEM_DEVICE_UID: DeviceId = 0x5359_5354_454d_0001;
pub const ENTITY_ID_SOURCE: EntityId = u64::MAX;

pub const ISSUER_SYSTEM: IssuerId = 2;
pub const ISSUER_ZIGBEE: IssuerId = 1;
pub const SOURCE_ZIGBEE: SourceId = 1;
pub const SOURCE_UI: SourceId = 2;
pub const SOURCE_AUTOMATION: SourceId = 3;
pub const SOURCE_SYSTEM: SourceId = 4;
pub const SOURCE_WIFI: SourceId = 5;

/// Synthetic system entity (V2 `ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM)`).
pub const ENTITY_SYSTEM: EntityId = 0x9229_b477_04a2_45b5;
/// Synthetic weather record key (ASCII "WTHR" in the high bytes).
pub const WEATHER_DEVICE_UID: DeviceId = 0x5754_4852_0000_0001;
/// Synthetic Wi-Fi uid (ASCII "WIFI" in the high bytes).
pub const WIFI_DEVICE_UID: DeviceId = 0x5749_4649_0000_0001;

pub const ENTITY_AUTOMATION: u32 = 4;
pub const ENTITY_LOCATION: u32 = 6;
pub const ENTITY_GROUP: u32 = 7;
pub const ENTITY_GROUP_ITEM: u32 = 8;
pub const ENTITY_WEATHER: u32 = 9;
pub const ENTITY_WIFI_SCAN: u32 = 10;
pub const ENTITY_WIFI_KNOWN: u32 = 11;
pub const ENTITY_WIFI_STATUS: u32 = 12;
pub const ENTITY_SETTINGS: u32 = 13;
pub const ENTITY_ENTITY: u32 = 14;
pub const ENTITY_PROPERTY_STATE: u32 = 15;
pub const ENTITY_CAPABILITIES: u32 = 16;
pub const ENTITY_LOGICAL_DEVICE: u32 = 17;
pub const ENTITY_GROUP_AUTOMATION: u32 = 18;

pub const PROPERTY_POWER: PropertyId = 1;
pub const PROPERTY_BRIGHTNESS: PropertyId = 2;
pub const PROPERTY_COLOR_HUE: PropertyId = 3;
pub const PROPERTY_COLOR_SATURATION: PropertyId = 4;
pub const PROPERTY_COLOR_X: PropertyId = 5;
pub const PROPERTY_COLOR_Y: PropertyId = 6;
pub const PROPERTY_COLOR_TEMPERATURE: PropertyId = 7;
pub const PROPERTY_TEMPERATURE: PropertyId = 8;
pub const PROPERTY_HUMIDITY: PropertyId = 9;
pub const PROPERTY_ILLUMINANCE: PropertyId = 10;
pub const PROPERTY_OCCUPANCY: PropertyId = 11;
pub const PROPERTY_BATTERY_VOLTAGE: PropertyId = 12;
pub const PROPERTY_BATTERY_PERCENT: PropertyId = 13;
pub const PROPERTY_SYSTEM_TIME_UTC: PropertyId = 14;
pub const PROPERTY_SYSTEM_TIME_STATUS: PropertyId = 15;
pub const PROPERTY_SYSTEM_HOUR: PropertyId = 16;
pub const PROPERTY_SYSTEM_MINUTE: PropertyId = 17;
pub const PROPERTY_SYSTEM_WEEKDAY: PropertyId = 18;
pub const PROPERTY_SYSTEM_WEEKDAY_MASK: PropertyId = 19;
pub const PROPERTY_SYSTEM_MINUTES_OF_DAY: PropertyId = 20;
pub const PROPERTY_SYSTEM_TZ_OFFSET: PropertyId = 21;
pub const PROPERTY_COLOR: PropertyId = 22;

pub const ACTION_ON: ActionId = 1;
pub const ACTION_OFF: ActionId = 2;
pub const ACTION_TOGGLE: ActionId = 3;
pub const ACTION_SET: ActionId = 4;

pub const COMMAND_DISCOVERY: u32 = 3;
pub const COMMAND_PROPERTY: u32 = 6;
pub const COMMAND_WIFI_SCAN: u32 = 4;
pub const COMMAND_WIFI_CONNECT: u32 = 5;
pub const COMMAND_AUTOMATION_ENABLE: u32 = 7;
pub const COMMAND_SETTINGS: u32 = 8;
pub const COMMAND_RECORD_LEN: usize = 32;

pub const EVENT_SINGLE_PRESS: EventId = 1;
pub const EVENT_DOUBLE_PRESS: EventId = 2;
pub const EVENT_HOLD: EventId = 3;
pub const EVENT_MINUTE_TICK: EventId = 4;
pub const EVENT_HALF_HOUR_TICK: EventId = 5;
pub const EVENT_HOUR_TICK: EventId = 6;
pub const EVENT_DAY_TICK: EventId = 7;
pub const EVENT_WEATHER_CHANGED: EventId = 8;

pub const ENTITY_NAME_MAX: usize = 32;
pub const NATIVE_ID_MAX: usize = 32;
pub const SEED_MAX: usize = 64;
pub const CAPABILITIES_MAX: usize = 8;
pub const CAPABILITY_ACTIONS_MAX: usize = 4;
pub const CAPABILITY_EVENTS_MAX: usize = 4;
pub const CAPABILITY_LEN: usize = 24;
pub const CAPABILITIES_LEN: usize = 216;
pub const ENTITY_RECORD_LEN: usize = 40;
pub const DEVICE_RECORD_LEN: usize = 80;
pub const STATE_KEY_LEN: usize = 16;
pub const STATE_RECORD_LEN: usize = 8;
pub const GROUP_KEY_LEN: usize = 8;
pub const GROUP_RECORD_LEN: usize = 32;
pub const GROUP_TITLE_MAX: usize = 32;
pub const GROUP_ITEM_KEY_LEN: usize = 16;
pub const GROUP_ITEM_RECORD_LEN: usize = 36;
pub const GROUP_ITEM_TITLE_MAX: usize = 32;
pub const GROUP_AUTOMATION_KEY_LEN: usize = 16;
pub const AUTOMATION_KEY_LEN: usize = 8;
pub const AUTOMATION_RECORD_LEN: usize = 456;

pub const RULE_CONDITIONS_MAX: usize = 4;
pub const RULE_TRIGGERS_MAX: usize = 4;
pub const RULE_ACTIONS_MAX: usize = 4;
pub const RULE_NAME_MAX: usize = 64;
pub const RULE_REF_LEN: usize = 16;
pub const RULE_COMPARE_LEN: usize = 12;
pub const RULE_CONDITION_LEN: usize = 32;
pub const RULE_TRIGGER_LEN: usize = 32;
pub const RULE_ACTION_LEN: usize = 32;
pub const RULE_COMMAND_VALUE_LEN: usize = 12;
pub const RULE_VALUE_LEN: usize = 8;
pub const RULE_RECORD_LEN: usize = AUTOMATION_RECORD_LEN;

pub const RULE_TRIGGERS_OFFSET: usize = 0;
pub const RULE_ACTIONS_OFFSET: usize = 128;
pub const RULE_CONDITIONS_OFFSET: usize = 256;
pub const RULE_ENABLED_OFFSET: usize = 384;
pub const RULE_TRIGGER_COUNT_OFFSET: usize = 385;
pub const RULE_ACTION_COUNT_OFFSET: usize = 386;
pub const RULE_CONDITIONS_COUNT_OFFSET: usize = 387;
pub const RULE_RESERVED_OFFSET: usize = 388;
pub const RULE_NAME_OFFSET: usize = 392;

pub const RULE_REF_ENTITY_OFFSET: usize = 0;
pub const RULE_REF_PROPERTY_OFFSET: usize = 8;
pub const RULE_REF_RESERVED_OFFSET: usize = 12;

pub const RULE_COMPARE_OP_OFFSET: usize = 0;
pub const RULE_COMPARE_RESERVED_OFFSET: usize = 1;
pub const RULE_COMPARE_VALUE_OFFSET: usize = 4;
pub const RULE_COMPARE_VALUE2_OFFSET: usize = 8;

pub const RULE_CONDITION_REF_OFFSET: usize = 0;
pub const RULE_CONDITION_COMPARE_OFFSET: usize = 16;
pub const RULE_CONDITION_RESERVED_OFFSET: usize = 28;

pub const RULE_TRIGGER_ENTITY_OFFSET: usize = 0;
pub const RULE_TRIGGER_EVENT_ID_OFFSET: usize = 8;
pub const RULE_TRIGGER_PROPERTY_OFFSET: usize = 12;
pub const RULE_TRIGGER_VALUE_OFFSET: usize = 16;
pub const RULE_TRIGGER_VALUE2_OFFSET: usize = 20;
pub const RULE_TRIGGER_MINUTES_OFFSET: usize = 24;
pub const RULE_TRIGGER_KIND_OFFSET: usize = 26;
pub const RULE_TRIGGER_OP_OFFSET: usize = 27;
pub const RULE_TRIGGER_EDGE_OFFSET: usize = 28;
pub const RULE_TRIGGER_WEEKDAY_OFFSET: usize = 29;
pub const RULE_TRIGGER_RESERVED_OFFSET: usize = 30;

pub const RULE_ACTION_REF_OFFSET: usize = 0;
pub const RULE_ACTION_VALUE_OFFSET: usize = 16;
pub const RULE_ACTION_ACTION_OFFSET: usize = 28;
pub const RULE_ACTION_RESERVED_OFFSET: usize = 29;

pub const RULE_COMMAND_KIND_OFFSET: usize = 0;
pub const RULE_COMMAND_UNION_OFFSET: usize = 4;
pub const RULE_COMMAND_SCALAR_KIND_OFFSET: usize = 4;
pub const RULE_COMMAND_SCALAR_VALUE_OFFSET: usize = 8;
pub const RULE_COMMAND_XY_X_OFFSET: usize = 4;
pub const RULE_COMMAND_XY_Y_OFFSET: usize = 8;

pub const GROUP_KEY_ID_OFFSET: usize = 0;
pub const GROUP_ITEM_GROUP_OFFSET: usize = 0;
pub const GROUP_ITEM_ENTITY_OFFSET: usize = 8;
pub const GROUP_ITEM_ORDER_OFFSET: usize = 0;
pub const GROUP_ITEM_RESERVED_OFFSET: usize = 2;
pub const GROUP_ITEM_TITLE_OFFSET: usize = 4;
pub const GROUP_AUTOMATION_GROUP_OFFSET: usize = 0;
pub const GROUP_AUTOMATION_AUTOMATION_OFFSET: usize = 8;
pub const AUTOMATION_KEY_ID_OFFSET: usize = 0;

const _: () = {
    assert!(AUTOMATION_RECORD_LEN == 456);
    assert!(RULE_RECORD_LEN == 456);
    assert!(RULE_REF_LEN == 16);
    assert!(RULE_COMPARE_LEN == 12);
    assert!(RULE_CONDITION_LEN == 32);
    assert!(RULE_TRIGGER_LEN == 32);
    assert!(RULE_ACTION_LEN == 32);
    assert!(RULE_COMMAND_VALUE_LEN == 12);
    assert!(RULE_VALUE_LEN == 8);
    assert!(RULE_TRIGGERS_OFFSET + RULE_TRIGGERS_MAX * RULE_TRIGGER_LEN == RULE_ACTIONS_OFFSET);
    assert!(RULE_ACTIONS_OFFSET + RULE_ACTIONS_MAX * RULE_ACTION_LEN == RULE_CONDITIONS_OFFSET);
    assert!(
        RULE_CONDITIONS_OFFSET + RULE_CONDITIONS_MAX * RULE_CONDITION_LEN == RULE_ENABLED_OFFSET
    );
    assert!(RULE_ENABLED_OFFSET + 4 == RULE_RESERVED_OFFSET);
    assert!(RULE_RESERVED_OFFSET + 4 == RULE_NAME_OFFSET);
    assert!(RULE_NAME_OFFSET + RULE_NAME_MAX == RULE_RECORD_LEN);
    assert!(RULE_REF_ENTITY_OFFSET + 8 == RULE_REF_PROPERTY_OFFSET);
    assert!(RULE_REF_PROPERTY_OFFSET + 4 == RULE_REF_RESERVED_OFFSET);
    assert!(RULE_REF_RESERVED_OFFSET + 4 == RULE_REF_LEN);
    assert!(RULE_COMPARE_OP_OFFSET + 1 == RULE_COMPARE_RESERVED_OFFSET);
    assert!(RULE_COMPARE_RESERVED_OFFSET + 3 == RULE_COMPARE_VALUE_OFFSET);
    assert!(RULE_COMPARE_VALUE_OFFSET + 4 == RULE_COMPARE_VALUE2_OFFSET);
    assert!(RULE_COMPARE_VALUE2_OFFSET + 4 == RULE_COMPARE_LEN);
    assert!(RULE_CONDITION_REF_OFFSET + RULE_REF_LEN == RULE_CONDITION_COMPARE_OFFSET);
    assert!(RULE_CONDITION_COMPARE_OFFSET + RULE_COMPARE_LEN == RULE_CONDITION_RESERVED_OFFSET);
    assert!(RULE_CONDITION_RESERVED_OFFSET + 4 == RULE_CONDITION_LEN);
    assert!(RULE_TRIGGER_ENTITY_OFFSET + 8 == RULE_TRIGGER_EVENT_ID_OFFSET);
    assert!(RULE_TRIGGER_EVENT_ID_OFFSET + 4 == RULE_TRIGGER_PROPERTY_OFFSET);
    assert!(RULE_TRIGGER_PROPERTY_OFFSET + 4 == RULE_TRIGGER_VALUE_OFFSET);
    assert!(RULE_TRIGGER_VALUE_OFFSET + 4 == RULE_TRIGGER_VALUE2_OFFSET);
    assert!(RULE_TRIGGER_VALUE2_OFFSET + 4 == RULE_TRIGGER_MINUTES_OFFSET);
    assert!(RULE_TRIGGER_MINUTES_OFFSET + 2 == RULE_TRIGGER_KIND_OFFSET);
    assert!(RULE_TRIGGER_KIND_OFFSET + 1 == RULE_TRIGGER_OP_OFFSET);
    assert!(RULE_TRIGGER_OP_OFFSET + 1 == RULE_TRIGGER_EDGE_OFFSET);
    assert!(RULE_TRIGGER_EDGE_OFFSET + 1 == RULE_TRIGGER_WEEKDAY_OFFSET);
    assert!(RULE_TRIGGER_WEEKDAY_OFFSET + 1 == RULE_TRIGGER_RESERVED_OFFSET);
    assert!(RULE_TRIGGER_RESERVED_OFFSET + 2 == RULE_TRIGGER_LEN);
    assert!(RULE_ACTION_REF_OFFSET + RULE_REF_LEN == RULE_ACTION_VALUE_OFFSET);
    assert!(RULE_ACTION_VALUE_OFFSET + RULE_COMMAND_VALUE_LEN == RULE_ACTION_ACTION_OFFSET);
    assert!(RULE_ACTION_ACTION_OFFSET + 1 == RULE_ACTION_RESERVED_OFFSET);
    assert!(RULE_ACTION_RESERVED_OFFSET + 3 == RULE_ACTION_LEN);
    assert!(RULE_COMMAND_KIND_OFFSET + 4 == RULE_COMMAND_UNION_OFFSET);
    assert!(RULE_COMMAND_SCALAR_KIND_OFFSET + 4 == RULE_COMMAND_SCALAR_VALUE_OFFSET);
    assert!(RULE_COMMAND_SCALAR_VALUE_OFFSET + 4 == RULE_COMMAND_VALUE_LEN);
    assert!(RULE_COMMAND_XY_X_OFFSET + 4 == RULE_COMMAND_XY_Y_OFFSET);
    assert!(RULE_COMMAND_XY_Y_OFFSET + 4 == RULE_COMMAND_VALUE_LEN);
    assert!(GROUP_KEY_LEN == 8);
    assert!(GROUP_RECORD_LEN == 32);
    assert!(GROUP_TITLE_MAX == 32);
    assert!(GROUP_ITEM_KEY_LEN == 16);
    assert!(GROUP_ITEM_RECORD_LEN == 36);
    assert!(GROUP_ITEM_TITLE_MAX == 32);
    assert!(GROUP_AUTOMATION_KEY_LEN == 16);
    assert!(AUTOMATION_KEY_LEN == 8);
    assert!(GROUP_KEY_ID_OFFSET + 8 == GROUP_KEY_LEN);
    assert!(GROUP_ITEM_GROUP_OFFSET + 8 == GROUP_ITEM_ENTITY_OFFSET);
    assert!(GROUP_ITEM_ENTITY_OFFSET + 8 == GROUP_ITEM_KEY_LEN);
    assert!(GROUP_ITEM_ORDER_OFFSET + 2 == GROUP_ITEM_RESERVED_OFFSET);
    assert!(GROUP_ITEM_RESERVED_OFFSET + 2 == GROUP_ITEM_TITLE_OFFSET);
    assert!(GROUP_ITEM_TITLE_OFFSET + GROUP_ITEM_TITLE_MAX == GROUP_ITEM_RECORD_LEN);
    assert!(GROUP_AUTOMATION_GROUP_OFFSET + 8 == GROUP_AUTOMATION_AUTOMATION_OFFSET);
    assert!(GROUP_AUTOMATION_AUTOMATION_OFFSET + 8 == GROUP_AUTOMATION_KEY_LEN);
};
