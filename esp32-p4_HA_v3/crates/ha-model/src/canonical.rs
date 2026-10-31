//! Canonical HA model, ported from V2 `ha_model` with the same numbers and
//! record layouts. Domain stores these records but does not interpret them.

use crate::{Value, ValueType};

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

pub const COMMAND_PROPERTY: u32 = 6;
pub const COMMAND_WIFI_SCAN: u32 = 4;
pub const COMMAND_WIFI_CONNECT: u32 = 5;
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
pub const LOCATION_KEY_LEN: usize = 8;
pub const LOCATION_RECORD_LEN: usize = 60;
pub const LOCATION_NAME_MAX: usize = 48;
pub const WEATHER_RECORD_LEN: usize = 16;
pub const WIFI_SSID_MAX: usize = 32;
pub const WIFI_PASSWORD_MAX: usize = 64;
pub const WIFI_SCAN_MAX: usize = 16;
pub const WIFI_KNOWN_MAX: usize = 8;
pub const WIFI_SCAN_RECORD_LEN: usize = 4 + WIFI_SCAN_MAX * 36;
pub const WIFI_KNOWN_RECORD_LEN: usize = 96;
pub const WIFI_STATUS_RECORD_LEN: usize = 36;
pub const SETTINGS_KEY_LEN: usize = 1;
pub const SETTINGS_RECORD_LEN: usize = 8;
pub const SETTINGS_ID: u8 = 0;

/// `ha_settings_record_t` (8): timeout u32@0, brightness u8@4, palette u8@5.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SettingsRecord {
    pub screensaver_timeout_ms: u32,
    pub brightness_pct: u8,
    pub palette_id: u8,
}

impl Default for SettingsRecord {
    fn default() -> Self {
        Self {
            screensaver_timeout_ms: 0,
            brightness_pct: 80,
            palette_id: 0,
        }
    }
}

impl SettingsRecord {
    pub fn encode(self) -> [u8; SETTINGS_RECORD_LEN] {
        let mut out = [0u8; SETTINGS_RECORD_LEN];
        out[..4].copy_from_slice(&self.screensaver_timeout_ms.to_le_bytes());
        out[4] = self.brightness_pct;
        out[5] = self.palette_id;
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < SETTINGS_RECORD_LEN {
            return None;
        }
        Some(Self {
            screensaver_timeout_ms: u32::from_le_bytes(bytes[..4].try_into().ok()?),
            brightness_pct: bytes[4],
            palette_id: bytes[5],
        })
    }
}

/* Weather conditions (V2 ha_weather_condition_t). */
pub const WEATHER_UNKNOWN: u8 = 0;
pub const WEATHER_CLEAR: u8 = 1;
pub const WEATHER_CLEAR_NIGHT: u8 = 2;
pub const WEATHER_PARTLYCLOUDY: u8 = 3;
pub const WEATHER_CLOUDY: u8 = 4;
pub const WEATHER_OVERCAST: u8 = 5;
pub const WEATHER_FOG: u8 = 6;
pub const WEATHER_RAINY: u8 = 7;
pub const WEATHER_POURING: u8 = 8;
pub const WEATHER_SNOWY: u8 = 9;
pub const WEATHER_SNOWY_RAINY: u8 = 10;
pub const WEATHER_HAIL: u8 = 11;
pub const WEATHER_LIGHTNING: u8 = 12;
pub const WEATHER_LIGHTNING_RAINY: u8 = 13;
pub const WEATHER_WINDY: u8 = 14;
pub const WEATHER_WINDY_VARIANT: u8 = 15;
pub const WEATHER_EXCEPTIONAL: u8 = 16;

/// Fixed project key, byte-for-byte from V2 `ha_entity.c`.
pub const SIPHASH_KEY: [u8; 16] = [
    0x45, 0x53, 0x50, 0x33, 0x32, 0x2d, 0x50, 0x34, 0x2d, 0x48, 0x41, 0x2d, 0x76, 0x32, 0x21, 0x00,
];

fn rotl(x: u64, b: u32) -> u64 {
    x.rotate_left(b)
}

fn sip_round(v: &mut [u64; 4]) {
    v[0] = v[0].wrapping_add(v[1]);
    v[1] = rotl(v[1], 13);
    v[1] ^= v[0];
    v[0] = rotl(v[0], 32);
    v[2] = v[2].wrapping_add(v[3]);
    v[3] = rotl(v[3], 16);
    v[3] ^= v[2];
    v[0] = v[0].wrapping_add(v[3]);
    v[3] = rotl(v[3], 21);
    v[3] ^= v[0];
    v[2] = v[2].wrapping_add(v[1]);
    v[1] = rotl(v[1], 17);
    v[1] ^= v[2];
    v[2] = rotl(v[2], 32);
}

pub fn siphash24(key: &[u8; 16], data: &[u8]) -> u64 {
    let k0 = u64::from_le_bytes(key[..8].try_into().unwrap());
    let k1 = u64::from_le_bytes(key[8..].try_into().unwrap());
    let mut v = [
        k0 ^ 0x736f_6d65_7073_6575,
        k1 ^ 0x646f_7261_6e64_6f6d,
        k0 ^ 0x6c79_6765_6e65_7261,
        k1 ^ 0x7465_6462_7974_6573,
    ];
    let left = data.len() & 7;
    let end = data.len() - left;
    let mut b = (data.len() as u64) << 56;
    for chunk in data[..end].chunks(8) {
        let m = u64::from_le_bytes(chunk.try_into().unwrap());
        v[3] ^= m;
        sip_round(&mut v);
        sip_round(&mut v);
        v[0] ^= m;
    }
    for (i, byte) in data[end..].iter().enumerate() {
        b |= u64::from(*byte) << (8 * i);
    }
    v[3] ^= b;
    sip_round(&mut v);
    sip_round(&mut v);
    v[0] ^= b;
    v[2] ^= 0xff;
    for _ in 0..4 {
        sip_round(&mut v);
    }
    v[0] ^ v[1] ^ v[2] ^ v[3]
}

pub fn entity_id_derive(issuer: IssuerId, seed: &[u8]) -> EntityId {
    if seed.len() > SEED_MAX {
        return ENTITY_ID_NONE;
    }
    let mut data = [0u8; 8 + SEED_MAX];
    data[..4].copy_from_slice(&issuer.to_le_bytes());
    data[4..8].copy_from_slice(&(seed.len() as u32).to_le_bytes());
    data[8..8 + seed.len()].copy_from_slice(seed);
    let id = siphash24(&SIPHASH_KEY, &data[..8 + seed.len()]);
    if id == ENTITY_ID_NONE {
        1
    } else {
        id
    }
}

pub fn device_id_derive(source: SourceId, native_id: &[u8]) -> DeviceId {
    if native_id.is_empty() || native_id.len() > SEED_MAX {
        return DEVICE_ID_NONE;
    }
    entity_id_derive(source, native_id)
}

pub fn zigbee_seed(uid: u64, endpoint: u8) -> [u8; 9] {
    let mut seed = [0u8; 9];
    seed[..8].copy_from_slice(&uid.to_le_bytes());
    seed[8] = endpoint;
    seed
}

pub fn zigbee_entity_id(uid: u64, endpoint: u8) -> EntityId {
    entity_id_derive(ISSUER_ZIGBEE, &zigbee_seed(uid, endpoint))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EntityKey {
    pub id: EntityId,
}

impl EntityKey {
    pub fn to_bytes(self) -> [u8; 8] {
        self.id.to_le_bytes()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EntityRecord {
    pub device: DeviceId,
    pub name: [u8; ENTITY_NAME_MAX],
}

impl EntityRecord {
    pub fn new(device: DeviceId, name: &str) -> Self {
        let mut out = [0u8; ENTITY_NAME_MAX];
        let bytes = name.as_bytes();
        let len = bytes.len().min(ENTITY_NAME_MAX);
        out[..len].copy_from_slice(&bytes[..len]);
        Self { device, name: out }
    }

    pub fn encode(self) -> [u8; ENTITY_RECORD_LEN] {
        let mut out = [0u8; ENTITY_RECORD_LEN];
        out[..8].copy_from_slice(&self.device.to_le_bytes());
        out[8..].copy_from_slice(&self.name);
        out
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LogicalDeviceRecord {
    pub parent: DeviceId,
    pub source: SourceId,
    pub native_id: [u8; NATIVE_ID_MAX],
    pub name: [u8; NATIVE_ID_MAX],
}

impl LogicalDeviceRecord {
    pub fn new(source: SourceId, parent: DeviceId, native_id: &str, name: &str) -> Self {
        let mut native = [0u8; NATIVE_ID_MAX];
        let mut label = [0u8; NATIVE_ID_MAX];
        let n = native_id.as_bytes();
        let l = name.as_bytes();
        native[..n.len().min(NATIVE_ID_MAX)].copy_from_slice(&n[..n.len().min(NATIVE_ID_MAX)]);
        label[..l.len().min(NATIVE_ID_MAX)].copy_from_slice(&l[..l.len().min(NATIVE_ID_MAX)]);
        Self {
            parent,
            source,
            native_id: native,
            name: label,
        }
    }

    pub fn encode(self) -> [u8; DEVICE_RECORD_LEN] {
        let mut out = [0u8; DEVICE_RECORD_LEN];
        out[..8].copy_from_slice(&self.parent.to_le_bytes());
        out[8..12].copy_from_slice(&self.source.to_le_bytes());
        out[12..44].copy_from_slice(&self.native_id);
        out[44..76].copy_from_slice(&self.name);
        out
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StateKey {
    pub entity: EntityId,
    pub property: PropertyId,
}

impl StateKey {
    pub fn to_bytes(self) -> [u8; STATE_KEY_LEN] {
        let mut out = [0u8; STATE_KEY_LEN];
        out[..8].copy_from_slice(&self.entity.to_le_bytes());
        out[8..12].copy_from_slice(&self.property.to_le_bytes());
        out
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StateRecord {
    pub value: Value,
}

impl StateRecord {
    pub fn encode(self) -> [u8; STATE_RECORD_LEN] {
        self.value.to_bytes()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Capability {
    pub property: PropertyId,
    pub actions: [ActionId; CAPABILITY_ACTIONS_MAX],
    pub action_count: u8,
}

impl Capability {
    pub const fn read(property: PropertyId) -> Self {
        Self {
            property,
            actions: [0; CAPABILITY_ACTIONS_MAX],
            action_count: 0,
        }
    }

    pub fn with_actions(property: PropertyId, actions: &[ActionId]) -> Self {
        let mut list = [0; CAPABILITY_ACTIONS_MAX];
        let len = actions.len().min(CAPABILITY_ACTIONS_MAX);
        list[..len].copy_from_slice(&actions[..len]);
        Self {
            property,
            actions: list,
            action_count: len as u8,
        }
    }

    fn encode(self, out: &mut [u8]) {
        out[..4].copy_from_slice(&self.property.to_le_bytes());
        out[4] = self.action_count;
        for (index, action) in self.actions.iter().enumerate() {
            out[8 + index * 4..12 + index * 4].copy_from_slice(&action.to_le_bytes());
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CapabilitiesRecord {
    pub properties: [Capability; CAPABILITIES_MAX],
    pub property_count: u8,
    pub events: [EventId; CAPABILITY_EVENTS_MAX],
    pub event_count: u8,
}

impl Default for CapabilitiesRecord {
    fn default() -> Self {
        Self {
            properties: [Capability::read(0); CAPABILITIES_MAX],
            property_count: 0,
            events: [0; CAPABILITY_EVENTS_MAX],
            event_count: 0,
        }
    }
}

impl CapabilitiesRecord {
    pub fn push(&mut self, capability: Capability) -> bool {
        if usize::from(self.property_count) >= CAPABILITIES_MAX {
            return false;
        }
        self.properties[usize::from(self.property_count)] = capability;
        self.property_count += 1;
        true
    }

    pub fn push_event(&mut self, event: EventId) -> bool {
        if usize::from(self.event_count) >= CAPABILITY_EVENTS_MAX {
            return false;
        }
        self.events[usize::from(self.event_count)] = event;
        self.event_count += 1;
        true
    }

    pub fn encode(self) -> [u8; CAPABILITIES_LEN] {
        let mut out = [0u8; CAPABILITIES_LEN];
        out[0] = self.property_count;
        for (index, capability) in self.properties.iter().enumerate() {
            let start = 4 + index * CAPABILITY_LEN;
            capability.encode(&mut out[start..start + CAPABILITY_LEN]);
        }
        let events = 4 + CAPABILITIES_MAX * CAPABILITY_LEN;
        out[events] = self.event_count;
        for (index, event) in self.events.iter().enumerate() {
            let start = events + 4 + index * 4;
            out[start..start + 4].copy_from_slice(&event.to_le_bytes());
        }
        out
    }
}

/// `ha_location_record_t` (60): latitude f32@0, longitude f32@4, tz_offset_min
/// i16@8, flags u8@10 (bit0 tz_auto, bit1 pos_auto), name[48]@12.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LocationRecord {
    pub latitude: f32,
    pub longitude: f32,
    pub tz_offset_min: i16,
    pub tz_auto: bool,
    pub pos_auto: bool,
    pub name: [u8; LOCATION_NAME_MAX],
}

impl Default for LocationRecord {
    fn default() -> Self {
        Self {
            latitude: 0.0,
            longitude: 0.0,
            tz_offset_min: 0,
            tz_auto: true,
            pos_auto: true,
            name: [0; LOCATION_NAME_MAX],
        }
    }
}

impl LocationRecord {
    pub fn encode(self) -> [u8; LOCATION_RECORD_LEN] {
        let mut out = [0u8; LOCATION_RECORD_LEN];
        out[..4].copy_from_slice(&self.latitude.to_le_bytes());
        out[4..8].copy_from_slice(&self.longitude.to_le_bytes());
        out[8..10].copy_from_slice(&self.tz_offset_min.to_le_bytes());
        let mut flags = 0u8;
        if self.tz_auto {
            flags |= 1;
        }
        if self.pos_auto {
            flags |= 2;
        }
        out[10] = flags;
        out[12..].copy_from_slice(&self.name);
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < LOCATION_RECORD_LEN {
            return None;
        }
        let mut name = [0u8; LOCATION_NAME_MAX];
        name.copy_from_slice(&bytes[12..LOCATION_RECORD_LEN]);
        let flags = bytes[10];
        Some(Self {
            latitude: f32::from_le_bytes(bytes[..4].try_into().ok()?),
            longitude: f32::from_le_bytes(bytes[4..8].try_into().ok()?),
            tz_offset_min: i16::from_le_bytes(bytes[8..10].try_into().ok()?),
            tz_auto: flags & 1 != 0,
            pos_auto: flags & 2 != 0,
            name,
        })
    }

    /// Auto timezone is filled by the system service (GeoIP); with no data it is 0 (UTC).
    pub fn effective_tz_offset_min(self) -> i16 {
        self.tz_offset_min
    }
}

/// `ha_weather_record_t` (16): condition/cloud u8@0/1, temp i16@2, humidity u16@4,
/// pressure u16@6, wind u16@8, wind dir u16@10, reserved[4].
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WeatherRecord {
    pub condition: u8,
    pub cloud_pct: u8,
    pub temperature_c100: i16,
    pub humidity_p100: u16,
    pub pressure_hpa: u16,
    pub wind_kmh10: u16,
    pub wind_dir_deg: u16,
}

impl WeatherRecord {
    pub fn encode(self) -> [u8; WEATHER_RECORD_LEN] {
        let mut out = [0u8; WEATHER_RECORD_LEN];
        out[0] = self.condition;
        out[1] = self.cloud_pct;
        out[2..4].copy_from_slice(&self.temperature_c100.to_le_bytes());
        out[4..6].copy_from_slice(&self.humidity_p100.to_le_bytes());
        out[6..8].copy_from_slice(&self.pressure_hpa.to_le_bytes());
        out[8..10].copy_from_slice(&self.wind_kmh10.to_le_bytes());
        out[10..12].copy_from_slice(&self.wind_dir_deg.to_le_bytes());
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < WEATHER_RECORD_LEN {
            return None;
        }
        Some(Self {
            condition: bytes[0],
            cloud_pct: bytes[1],
            temperature_c100: i16::from_le_bytes(bytes[2..4].try_into().ok()?),
            humidity_p100: u16::from_le_bytes(bytes[4..6].try_into().ok()?),
            pressure_hpa: u16::from_le_bytes(bytes[6..8].try_into().ok()?),
            wind_kmh10: u16::from_le_bytes(bytes[8..10].try_into().ok()?),
            wind_dir_deg: u16::from_le_bytes(bytes[10..12].try_into().ok()?),
        })
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WifiScanAp {
    pub ssid: [u8; WIFI_SSID_MAX],
    pub rssi: i8,
    pub auth: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WifiScanRecord {
    pub aps: [WifiScanAp; WIFI_SCAN_MAX],
    pub count: u8,
}

impl Default for WifiScanRecord {
    fn default() -> Self {
        Self {
            aps: [WifiScanAp::default(); WIFI_SCAN_MAX],
            count: 0,
        }
    }
}

impl WifiScanRecord {
    pub fn encode(self) -> [u8; WIFI_SCAN_RECORD_LEN] {
        let mut out = [0u8; WIFI_SCAN_RECORD_LEN];
        out[0] = self.count.min(WIFI_SCAN_MAX as u8);
        for (index, ap) in self.aps.iter().take(usize::from(out[0])).enumerate() {
            let start = 4 + index * 36;
            out[start..start + 32].copy_from_slice(&ap.ssid);
            out[start + 32] = ap.rssi as u8;
            out[start + 33] = ap.auth;
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < WIFI_SCAN_RECORD_LEN {
            return None;
        }
        let mut record = Self::default();
        record.count = bytes[0].min(WIFI_SCAN_MAX as u8);
        for index in 0..usize::from(record.count) {
            let start = 4 + index * 36;
            record.aps[index]
                .ssid
                .copy_from_slice(&bytes[start..start + 32]);
            record.aps[index].rssi = bytes[start + 32] as i8;
            record.aps[index].auth = bytes[start + 33];
        }
        Some(record)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WifiStatusRecord {
    pub state: u8,
    pub connected: u8,
    pub rssi: i8,
    pub ssid: [u8; WIFI_SSID_MAX],
}

impl WifiStatusRecord {
    pub fn encode(self) -> [u8; WIFI_STATUS_RECORD_LEN] {
        let mut out = [0u8; WIFI_STATUS_RECORD_LEN];
        out[0] = self.state;
        out[1] = self.connected;
        out[2] = self.rssi as u8;
        out[4..].copy_from_slice(&self.ssid);
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < WIFI_STATUS_RECORD_LEN {
            return None;
        }
        Some(Self {
            state: bytes[0],
            connected: bytes[1],
            rssi: bytes[2] as i8,
            ssid: bytes[4..36].try_into().ok()?,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WifiKnownRecord {
    pub ssid: [u8; WIFI_SSID_MAX],
    pub password: [u8; WIFI_PASSWORD_MAX],
}

impl Default for WifiKnownRecord {
    fn default() -> Self {
        Self {
            ssid: [0; WIFI_SSID_MAX],
            password: [0; WIFI_PASSWORD_MAX],
        }
    }
}

impl WifiKnownRecord {
    pub fn encode(self) -> [u8; WIFI_KNOWN_RECORD_LEN] {
        let mut out = [0u8; WIFI_KNOWN_RECORD_LEN];
        out[..WIFI_SSID_MAX].copy_from_slice(&self.ssid);
        out[WIFI_SSID_MAX..].copy_from_slice(&self.password);
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < WIFI_KNOWN_RECORD_LEN {
            return None;
        }
        Some(Self {
            ssid: bytes[..WIFI_SSID_MAX].try_into().ok()?,
            password: bytes[WIFI_SSID_MAX..WIFI_KNOWN_RECORD_LEN]
                .try_into()
                .ok()?,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum CommandValue {
    None,
    Scalar(Value),
    Xy { x: f32, y: f32 },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CanonicalCommand {
    pub entity: EntityId,
    pub property: PropertyId,
    pub action: ActionId,
    pub value: CommandValue,
}

impl CanonicalCommand {
    /// V2 `ha_property_command_t`: entity u64@0, property u32@8, action u8@12,
    /// value kind u32@16, scalar kind / x @20, value / y @24.
    pub fn encode(self) -> [u8; COMMAND_RECORD_LEN] {
        let mut out = [0u8; COMMAND_RECORD_LEN];
        out[..8].copy_from_slice(&self.entity.to_le_bytes());
        out[8..12].copy_from_slice(&self.property.to_le_bytes());
        out[12] = self.action as u8;
        match self.value {
            CommandValue::None => {}
            CommandValue::Scalar(value) => {
                let bytes = value.to_bytes();
                out[16..20].copy_from_slice(&1u32.to_le_bytes());
                out[20..24].copy_from_slice(&u32::from(bytes[0]).to_le_bytes());
                out[24..28].copy_from_slice(&bytes[4..8]);
            }
            CommandValue::Xy { x, y } => {
                out[16..20].copy_from_slice(&2u32.to_le_bytes());
                out[20..24].copy_from_slice(&x.to_le_bytes());
                out[24..28].copy_from_slice(&y.to_le_bytes());
            }
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != COMMAND_RECORD_LEN {
            return None;
        }
        let entity = u64::from_le_bytes(bytes[..8].try_into().ok()?);
        let property = u32::from_le_bytes(bytes[8..12].try_into().ok()?);
        let action = u32::from(bytes[12]);
        let kind = u32::from_le_bytes(bytes[16..20].try_into().ok()?);
        let value = match kind {
            0 => CommandValue::None,
            1 => {
                let scalar = u32::from_le_bytes(bytes[20..24].try_into().ok()?);
                let bits = u32::from_le_bytes(bytes[24..28].try_into().ok()?);
                let value = match scalar {
                    1 => Value::bool(bits != 0),
                    2 => Value::i32(bits as i32),
                    3 => Value::u32(bits),
                    4 => Value::f32(f32::from_bits(bits)),
                    5 => Value::enum_(bits),
                    _ => return None,
                };
                CommandValue::Scalar(value)
            }
            2 => CommandValue::Xy {
                x: f32::from_le_bytes(bytes[20..24].try_into().ok()?),
                y: f32::from_le_bytes(bytes[24..28].try_into().ok()?),
            },
            _ => return None,
        };
        Some(Self {
            entity,
            property,
            action,
            value,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AbiError {
    InvalidSize,
    InvalidCount,
    NonZeroReserved,
    NonZeroUnused,
    InvalidKind,
}

pub type AbiResult<T> = core::result::Result<T, AbiError>;

fn abi_get_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn abi_get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn abi_get_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
        bytes[offset + 4],
        bytes[offset + 5],
        bytes[offset + 6],
        bytes[offset + 7],
    ])
}

fn abi_put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn abi_put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn abi_put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn abi_require_zero(bytes: &[u8], offset: usize, len: usize) -> AbiResult<()> {
    if bytes[offset..offset + len].iter().any(|byte| *byte != 0) {
        Err(AbiError::NonZeroReserved)
    } else {
        Ok(())
    }
}

fn abi_require_count(value: u8, max: usize) -> AbiResult<()> {
    if usize::from(value) > max {
        Err(AbiError::InvalidCount)
    } else {
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AutomationKey {
    pub id: u64,
}

impl AutomationKey {
    pub fn to_bytes(self) -> [u8; AUTOMATION_KEY_LEN] {
        self.id.to_le_bytes()
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != AUTOMATION_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            id: abi_get_u64(bytes, AUTOMATION_KEY_ID_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupKey {
    pub id: u64,
}

impl GroupKey {
    pub fn to_bytes(self) -> [u8; GROUP_KEY_LEN] {
        self.id.to_le_bytes()
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            id: abi_get_u64(bytes, GROUP_KEY_ID_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupRecord {
    pub title: [u8; GROUP_TITLE_MAX],
}

impl GroupRecord {
    pub fn encode(self) -> [u8; GROUP_RECORD_LEN] {
        self.title
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_RECORD_LEN {
            return Err(AbiError::InvalidSize);
        }
        let mut title = [0u8; GROUP_TITLE_MAX];
        title.copy_from_slice(&bytes[..GROUP_TITLE_MAX]);
        Ok(Self { title })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupItemKey {
    pub group_id: u64,
    pub entity: u64,
}

impl GroupItemKey {
    pub fn to_bytes(self) -> [u8; GROUP_ITEM_KEY_LEN] {
        let mut out = [0u8; GROUP_ITEM_KEY_LEN];
        abi_put_u64(&mut out, GROUP_ITEM_GROUP_OFFSET, self.group_id);
        abi_put_u64(&mut out, GROUP_ITEM_ENTITY_OFFSET, self.entity);
        out
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_ITEM_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            group_id: abi_get_u64(bytes, GROUP_ITEM_GROUP_OFFSET),
            entity: abi_get_u64(bytes, GROUP_ITEM_ENTITY_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupItemRecord {
    pub order: u16,
    pub title: [u8; GROUP_ITEM_TITLE_MAX],
}

impl GroupItemRecord {
    pub fn encode(self) -> [u8; GROUP_ITEM_RECORD_LEN] {
        let mut out = [0u8; GROUP_ITEM_RECORD_LEN];
        abi_put_u16(&mut out, GROUP_ITEM_ORDER_OFFSET, self.order);
        out[GROUP_ITEM_TITLE_OFFSET..].copy_from_slice(&self.title);
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_ITEM_RECORD_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(bytes, GROUP_ITEM_RESERVED_OFFSET, 2)?;
        let mut title = [0u8; GROUP_ITEM_TITLE_MAX];
        title.copy_from_slice(&bytes[GROUP_ITEM_TITLE_OFFSET..]);
        Ok(Self {
            order: abi_get_u16(bytes, GROUP_ITEM_ORDER_OFFSET),
            title,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GroupAutomationKey {
    pub group_id: u64,
    pub automation_id: u64,
}

impl GroupAutomationKey {
    pub fn to_bytes(self) -> [u8; GROUP_AUTOMATION_KEY_LEN] {
        let mut out = [0u8; GROUP_AUTOMATION_KEY_LEN];
        abi_put_u64(&mut out, GROUP_AUTOMATION_GROUP_OFFSET, self.group_id);
        abi_put_u64(
            &mut out,
            GROUP_AUTOMATION_AUTOMATION_OFFSET,
            self.automation_id,
        );
        out
    }

    pub fn from_bytes(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != GROUP_AUTOMATION_KEY_LEN {
            return Err(AbiError::InvalidSize);
        }
        Ok(Self {
            group_id: abi_get_u64(bytes, GROUP_AUTOMATION_GROUP_OFFSET),
            automation_id: abi_get_u64(bytes, GROUP_AUTOMATION_AUTOMATION_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RuleRef {
    pub entity: EntityId,
    pub property: PropertyId,
}

impl RuleRef {
    const EMPTY: Self = Self {
        entity: 0,
        property: 0,
    };

    pub fn encode(self) -> [u8; RULE_REF_LEN] {
        let mut out = [0u8; RULE_REF_LEN];
        abi_put_u64(&mut out, RULE_REF_ENTITY_OFFSET, self.entity);
        abi_put_u32(&mut out, RULE_REF_PROPERTY_OFFSET, self.property);
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_REF_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(bytes, RULE_REF_RESERVED_OFFSET, 4)?;
        Ok(Self {
            entity: abi_get_u64(bytes, RULE_REF_ENTITY_OFFSET),
            property: abi_get_u32(bytes, RULE_REF_PROPERTY_OFFSET),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleCompare {
    pub op: u8,
    pub value: f32,
    pub value2: f32,
}

impl RuleCompare {
    const EMPTY: Self = Self {
        op: 0,
        value: 0.0,
        value2: 0.0,
    };

    pub fn encode(self) -> [u8; RULE_COMPARE_LEN] {
        let mut out = [0u8; RULE_COMPARE_LEN];
        out[RULE_COMPARE_OP_OFFSET] = self.op;
        abi_put_u32(&mut out, RULE_COMPARE_VALUE_OFFSET, self.value.to_bits());
        abi_put_u32(&mut out, RULE_COMPARE_VALUE2_OFFSET, self.value2.to_bits());
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_COMPARE_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(bytes, RULE_COMPARE_RESERVED_OFFSET, 3)?;
        Ok(Self {
            op: bytes[RULE_COMPARE_OP_OFFSET],
            value: f32::from_bits(abi_get_u32(bytes, RULE_COMPARE_VALUE_OFFSET)),
            value2: f32::from_bits(abi_get_u32(bytes, RULE_COMPARE_VALUE2_OFFSET)),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleCondition {
    pub reference: RuleRef,
    pub compare: RuleCompare,
}

impl RuleCondition {
    const EMPTY: Self = Self {
        reference: RuleRef::EMPTY,
        compare: RuleCompare::EMPTY,
    };

    pub fn encode(self) -> [u8; RULE_CONDITION_LEN] {
        let mut out = [0u8; RULE_CONDITION_LEN];
        out[RULE_CONDITION_REF_OFFSET..RULE_CONDITION_REF_OFFSET + RULE_REF_LEN]
            .copy_from_slice(&self.reference.encode());
        out[RULE_CONDITION_COMPARE_OFFSET..RULE_CONDITION_COMPARE_OFFSET + RULE_COMPARE_LEN]
            .copy_from_slice(&self.compare.encode());
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_CONDITION_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(
            bytes,
            RULE_CONDITION_RESERVED_OFFSET,
            RULE_CONDITION_LEN - RULE_CONDITION_RESERVED_OFFSET,
        )?;
        Ok(Self {
            reference: RuleRef::decode(
                &bytes[RULE_CONDITION_REF_OFFSET..RULE_CONDITION_REF_OFFSET + RULE_REF_LEN],
            )?,
            compare: RuleCompare::decode(
                &bytes[RULE_CONDITION_COMPARE_OFFSET
                    ..RULE_CONDITION_COMPARE_OFFSET + RULE_COMPARE_LEN],
            )?,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleTrigger {
    pub entity: EntityId,
    pub event_id: EventId,
    pub property: PropertyId,
    pub value: f32,
    pub value2: f32,
    pub minutes_of_day: u16,
    pub kind: u8,
    pub op: u8,
    pub edge: u8,
    pub weekday_mask: u8,
}

impl RuleTrigger {
    const EMPTY: Self = Self {
        entity: 0,
        event_id: 0,
        property: 0,
        value: 0.0,
        value2: 0.0,
        minutes_of_day: 0,
        kind: 0,
        op: 0,
        edge: 0,
        weekday_mask: 0,
    };

    pub fn encode(self) -> [u8; RULE_TRIGGER_LEN] {
        let mut out = [0u8; RULE_TRIGGER_LEN];
        abi_put_u64(&mut out, RULE_TRIGGER_ENTITY_OFFSET, self.entity);
        abi_put_u32(&mut out, RULE_TRIGGER_EVENT_ID_OFFSET, self.event_id);
        abi_put_u32(&mut out, RULE_TRIGGER_PROPERTY_OFFSET, self.property);
        abi_put_u32(&mut out, RULE_TRIGGER_VALUE_OFFSET, self.value.to_bits());
        abi_put_u32(&mut out, RULE_TRIGGER_VALUE2_OFFSET, self.value2.to_bits());
        abi_put_u16(&mut out, RULE_TRIGGER_MINUTES_OFFSET, self.minutes_of_day);
        out[RULE_TRIGGER_KIND_OFFSET] = self.kind;
        out[RULE_TRIGGER_OP_OFFSET] = self.op;
        out[RULE_TRIGGER_EDGE_OFFSET] = self.edge;
        out[RULE_TRIGGER_WEEKDAY_OFFSET] = self.weekday_mask;
        out
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_TRIGGER_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(
            bytes,
            RULE_TRIGGER_RESERVED_OFFSET,
            RULE_TRIGGER_LEN - RULE_TRIGGER_RESERVED_OFFSET,
        )?;
        Ok(Self {
            entity: abi_get_u64(bytes, RULE_TRIGGER_ENTITY_OFFSET),
            event_id: abi_get_u32(bytes, RULE_TRIGGER_EVENT_ID_OFFSET),
            property: abi_get_u32(bytes, RULE_TRIGGER_PROPERTY_OFFSET),
            value: f32::from_bits(abi_get_u32(bytes, RULE_TRIGGER_VALUE_OFFSET)),
            value2: f32::from_bits(abi_get_u32(bytes, RULE_TRIGGER_VALUE2_OFFSET)),
            minutes_of_day: abi_get_u16(bytes, RULE_TRIGGER_MINUTES_OFFSET),
            kind: bytes[RULE_TRIGGER_KIND_OFFSET],
            op: bytes[RULE_TRIGGER_OP_OFFSET],
            edge: bytes[RULE_TRIGGER_EDGE_OFFSET],
            weekday_mask: bytes[RULE_TRIGGER_WEEKDAY_OFFSET],
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum RuleCommandValue {
    None,
    Scalar(Value),
    Xy { x: f32, y: f32 },
}

impl RuleCommandValue {
    pub fn encode(self) -> AbiResult<[u8; RULE_COMMAND_VALUE_LEN]> {
        let mut out = [0u8; RULE_COMMAND_VALUE_LEN];
        match self {
            Self::None => {}
            Self::Scalar(value) => {
                if value.kind == ValueType::None {
                    return Err(AbiError::InvalidKind);
                }
                abi_put_u32(&mut out, RULE_COMMAND_KIND_OFFSET, 1);
                abi_put_u32(&mut out, RULE_COMMAND_SCALAR_KIND_OFFSET, value.kind as u32);
                let bytes = value.to_bytes();
                out[RULE_COMMAND_SCALAR_VALUE_OFFSET..].copy_from_slice(&bytes[4..8]);
            }
            Self::Xy { x, y } => {
                abi_put_u32(&mut out, RULE_COMMAND_KIND_OFFSET, 2);
                abi_put_u32(&mut out, RULE_COMMAND_XY_X_OFFSET, x.to_bits());
                abi_put_u32(&mut out, RULE_COMMAND_XY_Y_OFFSET, y.to_bits());
            }
        }
        Ok(out)
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_COMMAND_VALUE_LEN {
            return Err(AbiError::InvalidSize);
        }
        match abi_get_u32(bytes, RULE_COMMAND_KIND_OFFSET) {
            0 => {
                abi_require_zero(
                    bytes,
                    RULE_COMMAND_UNION_OFFSET,
                    RULE_COMMAND_VALUE_LEN - RULE_COMMAND_UNION_OFFSET,
                )
                .map_err(|_| AbiError::NonZeroUnused)?;
                Ok(Self::None)
            }
            1 => {
                let scalar_kind = abi_get_u32(bytes, RULE_COMMAND_SCALAR_KIND_OFFSET);
                if !(1..=5).contains(&scalar_kind) {
                    return Err(AbiError::InvalidKind);
                }
                let bits = abi_get_u32(bytes, RULE_COMMAND_SCALAR_VALUE_OFFSET);
                let mut value = [0u8; RULE_VALUE_LEN];
                value[0] = scalar_kind as u8;
                value[4..8].copy_from_slice(&bits.to_le_bytes());
                Ok(Self::Scalar(
                    Value::from_bytes(&value).ok_or(AbiError::InvalidKind)?,
                ))
            }
            2 => Ok(Self::Xy {
                x: f32::from_bits(abi_get_u32(bytes, RULE_COMMAND_XY_X_OFFSET)),
                y: f32::from_bits(abi_get_u32(bytes, RULE_COMMAND_XY_Y_OFFSET)),
            }),
            _ => Err(AbiError::InvalidKind),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleAction {
    pub reference: RuleRef,
    pub value: RuleCommandValue,
    pub action: u8,
}

impl RuleAction {
    const EMPTY: Self = Self {
        reference: RuleRef::EMPTY,
        value: RuleCommandValue::None,
        action: 0,
    };

    pub fn encode(self) -> AbiResult<[u8; RULE_ACTION_LEN]> {
        let mut out = [0u8; RULE_ACTION_LEN];
        out[RULE_ACTION_REF_OFFSET..RULE_ACTION_REF_OFFSET + RULE_REF_LEN]
            .copy_from_slice(&self.reference.encode());
        out[RULE_ACTION_VALUE_OFFSET..RULE_ACTION_VALUE_OFFSET + RULE_COMMAND_VALUE_LEN]
            .copy_from_slice(&self.value.encode()?);
        out[RULE_ACTION_ACTION_OFFSET] = self.action;
        Ok(out)
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_ACTION_LEN {
            return Err(AbiError::InvalidSize);
        }
        abi_require_zero(
            bytes,
            RULE_ACTION_RESERVED_OFFSET,
            RULE_ACTION_LEN - RULE_ACTION_RESERVED_OFFSET,
        )?;
        Ok(Self {
            reference: RuleRef::decode(
                &bytes[RULE_ACTION_REF_OFFSET..RULE_ACTION_REF_OFFSET + RULE_REF_LEN],
            )?,
            value: RuleCommandValue::decode(
                &bytes[RULE_ACTION_VALUE_OFFSET..RULE_ACTION_VALUE_OFFSET + RULE_COMMAND_VALUE_LEN],
            )?,
            action: bytes[RULE_ACTION_ACTION_OFFSET],
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuleRecord {
    pub triggers: [RuleTrigger; RULE_TRIGGERS_MAX],
    pub actions: [RuleAction; RULE_ACTIONS_MAX],
    pub conditions: [RuleCondition; RULE_CONDITIONS_MAX],
    pub enabled: u8,
    pub trigger_count: u8,
    pub action_count: u8,
    pub conditions_count: u8,
    pub name: [u8; RULE_NAME_MAX],
}

impl RuleRecord {
    pub fn encode(self) -> AbiResult<[u8; RULE_RECORD_LEN]> {
        abi_require_count(self.trigger_count, RULE_TRIGGERS_MAX)?;
        abi_require_count(self.action_count, RULE_ACTIONS_MAX)?;
        abi_require_count(self.conditions_count, RULE_CONDITIONS_MAX)?;

        let mut out = [0u8; RULE_RECORD_LEN];
        for (index, trigger) in self.triggers.iter().enumerate() {
            let start = RULE_TRIGGERS_OFFSET + index * RULE_TRIGGER_LEN;
            out[start..start + RULE_TRIGGER_LEN].copy_from_slice(&trigger.encode());
        }
        for (index, action) in self.actions.iter().enumerate() {
            let start = RULE_ACTIONS_OFFSET + index * RULE_ACTION_LEN;
            out[start..start + RULE_ACTION_LEN].copy_from_slice(&action.encode()?);
        }
        for (index, condition) in self.conditions.iter().enumerate() {
            let start = RULE_CONDITIONS_OFFSET + index * RULE_CONDITION_LEN;
            out[start..start + RULE_CONDITION_LEN].copy_from_slice(&condition.encode());
        }
        out[RULE_ENABLED_OFFSET] = self.enabled;
        out[RULE_TRIGGER_COUNT_OFFSET] = self.trigger_count;
        out[RULE_ACTION_COUNT_OFFSET] = self.action_count;
        out[RULE_CONDITIONS_COUNT_OFFSET] = self.conditions_count;
        out[RULE_NAME_OFFSET..].copy_from_slice(&self.name);
        Ok(out)
    }

    pub fn decode(bytes: &[u8]) -> AbiResult<Self> {
        if bytes.len() != RULE_RECORD_LEN {
            return Err(AbiError::InvalidSize);
        }
        let trigger_count = bytes[RULE_TRIGGER_COUNT_OFFSET];
        let action_count = bytes[RULE_ACTION_COUNT_OFFSET];
        let conditions_count = bytes[RULE_CONDITIONS_COUNT_OFFSET];
        abi_require_count(trigger_count, RULE_TRIGGERS_MAX)?;
        abi_require_count(action_count, RULE_ACTIONS_MAX)?;
        abi_require_count(conditions_count, RULE_CONDITIONS_MAX)?;
        abi_require_zero(bytes, RULE_RESERVED_OFFSET, 4)?;

        let mut triggers = [RuleTrigger::EMPTY; RULE_TRIGGERS_MAX];
        for (index, slot) in triggers.iter_mut().enumerate() {
            let start = RULE_TRIGGERS_OFFSET + index * RULE_TRIGGER_LEN;
            *slot = RuleTrigger::decode(&bytes[start..start + RULE_TRIGGER_LEN])?;
        }
        let mut actions = [RuleAction::EMPTY; RULE_ACTIONS_MAX];
        for (index, slot) in actions.iter_mut().enumerate() {
            let start = RULE_ACTIONS_OFFSET + index * RULE_ACTION_LEN;
            *slot = RuleAction::decode(&bytes[start..start + RULE_ACTION_LEN])?;
        }
        let mut conditions = [RuleCondition::EMPTY; RULE_CONDITIONS_MAX];
        for (index, slot) in conditions.iter_mut().enumerate() {
            let start = RULE_CONDITIONS_OFFSET + index * RULE_CONDITION_LEN;
            *slot = RuleCondition::decode(&bytes[start..start + RULE_CONDITION_LEN])?;
        }
        let mut name = [0u8; RULE_NAME_MAX];
        name.copy_from_slice(&bytes[RULE_NAME_OFFSET..]);

        Ok(Self {
            triggers,
            actions,
            conditions,
            enabled: bytes[RULE_ENABLED_OFFSET],
            trigger_count,
            action_count,
            conditions_count,
            name,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn siphash_matches_official_vector() {
        let mut key = [0u8; 16];
        let mut msg = [0u8; 15];
        for (i, byte) in key.iter_mut().enumerate() {
            *byte = i as u8;
        }
        for (i, byte) in msg.iter_mut().enumerate() {
            *byte = i as u8;
        }
        assert_eq!(siphash24(&key, &msg), 0xa129_ca61_49be_45e5);
    }

    #[test]
    fn zigbee_entity_id_matches_v2_golden() {
        let id = zigbee_entity_id(0x0012_4B00_0A1B_2C3D, 1);
        assert_eq!(id, 0xdf80_3cba_f588_876d);
        assert_ne!(id, zigbee_entity_id(0x0012_4B00_0A1B_2C3D, 2));
        assert_ne!(entity_id_derive(ISSUER_SYSTEM, &zigbee_seed(1, 1)), id);
    }

    #[test]
    fn wifi_records_match_v2_layouts() {
        let mut scan = WifiScanRecord::default();
        scan.count = 1;
        scan.aps[0].ssid[..4].copy_from_slice(b"wifi");
        scan.aps[0].rssi = -54;
        scan.aps[0].auth = 2;
        let bytes = scan.encode();
        assert_eq!(bytes.len(), WIFI_SCAN_RECORD_LEN);
        assert_eq!(WifiScanRecord::decode(&bytes), Some(scan));

        let status = WifiStatusRecord {
            state: 3,
            connected: 1,
            rssi: -60,
            ssid: scan.aps[0].ssid,
        };
        let bytes = status.encode();
        assert_eq!(bytes.len(), WIFI_STATUS_RECORD_LEN);
        assert_eq!(WifiStatusRecord::decode(&bytes), Some(status));

        let known = WifiKnownRecord {
            ssid: scan.aps[0].ssid,
            password: [b'x'; WIFI_PASSWORD_MAX],
        };
        let bytes = known.encode();
        assert_eq!(bytes.len(), WIFI_KNOWN_RECORD_LEN);
        assert_eq!(WifiKnownRecord::decode(&bytes), Some(known));
    }

    #[test]
    fn source_and_command_ids_match_v2() {
        assert_eq!(SOURCE_UI, 2);
        assert_eq!(SOURCE_AUTOMATION, 3);
        assert_eq!(SOURCE_SYSTEM, 4);
        assert_eq!(SOURCE_WIFI, 5);
        assert_eq!(COMMAND_PROPERTY, 6);
    }

    #[test]
    fn property_command_roundtrips_in_v2_layout() {
        for value in [
            CommandValue::None,
            CommandValue::Scalar(Value::bool(true)),
            CommandValue::Scalar(Value::i32(-7)),
            CommandValue::Scalar(Value::u32(42)),
            CommandValue::Scalar(Value::f32(50.0)),
            CommandValue::Scalar(Value::enum_(3)),
            CommandValue::Xy { x: 0.25, y: 0.5 },
        ] {
            let command = CanonicalCommand {
                entity: 0x1122_3344_5566_7788,
                property: PROPERTY_BRIGHTNESS,
                action: ACTION_SET,
                value,
            };
            let bytes = command.encode();
            assert_eq!(bytes.len(), COMMAND_RECORD_LEN);
            assert_eq!(CanonicalCommand::decode(&bytes), Some(command));
        }
        assert!(CanonicalCommand::decode(&[0u8; COMMAND_RECORD_LEN - 1]).is_none());
    }

    #[test]
    fn records_have_v2_sizes_and_layouts() {
        let entity = EntityRecord::new(7, "Kitchen");
        assert_eq!(entity.encode().len(), ENTITY_RECORD_LEN);
        let device = LogicalDeviceRecord::new(SOURCE_ZIGBEE, 0, "00124b000a1b2c3d", "Plug");
        assert_eq!(device.encode().len(), DEVICE_RECORD_LEN);
        let key = StateKey {
            entity: 1,
            property: PROPERTY_POWER,
        };
        assert_eq!(key.to_bytes().len(), STATE_KEY_LEN);
        let record = StateRecord {
            value: Value::bool(true),
        };
        assert_eq!(record.encode().len(), STATE_RECORD_LEN);
        assert_eq!(record.encode()[0], 1);
        let mut caps = CapabilitiesRecord::default();
        assert!(caps.push(Capability::with_actions(
            PROPERTY_POWER,
            &[ACTION_ON, ACTION_OFF, ACTION_TOGGLE]
        )));
        assert!(caps.push_event(EVENT_SINGLE_PRESS));
        assert_eq!(caps.encode().len(), CAPABILITIES_LEN);
    }

    #[test]
    fn location_layout_and_flags_match_v2() {
        let mut name = [0u8; LOCATION_NAME_MAX];
        name[..6].copy_from_slice(b"Moscow");
        let location = LocationRecord {
            latitude: 1.5,
            longitude: -2.25,
            tz_offset_min: 180,
            tz_auto: false,
            pos_auto: false,
            name,
        };
        let bytes = location.encode();
        assert_eq!(bytes.len(), LOCATION_RECORD_LEN);
        assert_eq!(bytes[10], 0);
        assert_eq!(LocationRecord::decode(&bytes), Some(location));
        assert_eq!(location.effective_tz_offset_min(), 180);

        let auto = LocationRecord::default();
        assert_eq!(auto.encode()[10], 3);
        assert_eq!(auto.effective_tz_offset_min(), 0);
        assert!(LocationRecord::decode(&bytes[..59]).is_none());
    }

    #[test]
    fn weather_layout_matches_v2() {
        let weather = WeatherRecord {
            condition: WEATHER_PARTLYCLOUDY,
            cloud_pct: 40,
            temperature_c100: 2_150,
            humidity_p100: 6_100,
            pressure_hpa: 1_013,
            wind_kmh10: 125,
            wind_dir_deg: 270,
        };
        let bytes = weather.encode();
        assert_eq!(bytes.len(), WEATHER_RECORD_LEN);
        assert_eq!(&bytes[2..4], &2_150i16.to_le_bytes());
        assert_eq!(WeatherRecord::decode(&bytes), Some(weather));
        assert!(WeatherRecord::decode(&bytes[..15]).is_none());
    }
}
