use super::*;
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
pub const WIFI_CONNECT_ARGS_LEN: usize = WIFI_SSID_MAX + WIFI_PASSWORD_MAX;
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
    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < ENTITY_RECORD_LEN {
            return None;
        }
        let mut name = [0u8; ENTITY_NAME_MAX];
        name.copy_from_slice(&bytes[8..ENTITY_RECORD_LEN]);
        Some(Self {
            device: u64::from_le_bytes(bytes[..8].try_into().ok()?),
            name,
        })
    }

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
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < STATE_KEY_LEN {
            return None;
        }
        Some(Self {
            entity: u64::from_le_bytes(bytes[..8].try_into().ok()?),
            property: u32::from_le_bytes(bytes[8..12].try_into().ok()?),
        })
    }

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

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < CAPABILITIES_LEN {
            return None;
        }
        let events = 4 + CAPABILITIES_MAX * CAPABILITY_LEN;
        let mut record = Self {
            property_count: bytes[0].min(CAPABILITIES_MAX as u8),
            event_count: bytes[events].min(CAPABILITY_EVENTS_MAX as u8),
            ..Self::default()
        };
        for index in 0..usize::from(record.property_count) {
            let at = 4 + index * CAPABILITY_LEN;
            let mut capability =
                Capability::read(u32::from_le_bytes(bytes[at..at + 4].try_into().ok()?));
            capability.action_count = bytes[at + 4].min(CAPABILITY_ACTIONS_MAX as u8);
            for action in 0..usize::from(capability.action_count) {
                let offset = at + 8 + action * 4;
                capability.actions[action] =
                    u32::from_le_bytes(bytes[offset..offset + 4].try_into().ok()?);
            }
            record.properties[index] = capability;
        }
        for index in 0..usize::from(record.event_count) {
            let offset = events + 4 + index * 4;
            record.events[index] = u32::from_le_bytes(bytes[offset..offset + 4].try_into().ok()?);
        }
        Some(record)
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
        let mut record = Self {
            count: bytes[0].min(WIFI_SCAN_MAX as u8),
            ..Self::default()
        };
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

/// `ha_wifi_connect_args_t` (96): ssid[32], password[64].
pub fn encode_wifi_connect(ssid: &str, password: &str) -> [u8; WIFI_CONNECT_ARGS_LEN] {
    let mut out = [0u8; WIFI_CONNECT_ARGS_LEN];
    let ssid = ssid.as_bytes();
    let password = password.as_bytes();
    // Keep one byte for the NUL terminator so decode_wifi_connect can read it.
    let ssid_len = ssid.len().min(WIFI_SSID_MAX - 1);
    let password_len = password.len().min(WIFI_PASSWORD_MAX - 1);
    out[..ssid_len].copy_from_slice(&ssid[..ssid_len]);
    out[WIFI_SSID_MAX..WIFI_SSID_MAX + password_len]
        .copy_from_slice(&password[..password_len]);
    out
}

pub fn decode_wifi_connect(args: &[u8]) -> Option<(String, String)> {
    if args.len() != WIFI_CONNECT_ARGS_LEN {
        return None;
    }
    let ssid = std::ffi::CStr::from_bytes_until_nul(&args[..WIFI_SSID_MAX])
        .ok()?
        .to_string_lossy()
        .into_owned();
    let password = std::ffi::CStr::from_bytes_until_nul(&args[WIFI_SSID_MAX..])
        .ok()?
        .to_string_lossy()
        .into_owned();
    Some((ssid, password))
}
