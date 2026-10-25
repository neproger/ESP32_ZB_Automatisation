//! Portable port of Zigbee2MQTT's compatibility system.
//!
//! This crate knows Zigbee and its own vocabulary only: devices, profiles, ZCL
//! events, converters, commands and configure plans. It has no dependency on HA
//! Domain/MicroDB/protocol types, on ESP-IDF, or on any transport. The HA Zigbee
//! Layer is the single place that translates [`Update`] into canonical HA types.
//!
//! The Z2M side is data: profiles are generated offline from
//! `zigbee-herdsman-converters` (`extends`/`modernExtend` are flattened before
//! build). Unique logic is a small [`Converter::Custom`] handler, never a dropped
//! rule.
//!
//! Phase A: types, identification, `fromZigbee` decoding and `toZigbee` encoding
//! for fixed arguments. Tuya DP codecs, scaling variants and the configure state
//! machine land with the devices that need them; the contracts are already here.

mod frame;
mod generated;
mod matcher;
pub mod capabilities;

pub use frame::{AttributeFrameView, AttributeIter, AttributeView, FrameError};
pub use generated::*;
pub use matcher::*;

pub const MAX_UPDATES: usize = 8;

pub type DeviceKey = [u8; 8];

/// Opaque property id in the z2m-rs namespace (generator-assigned, stable).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PropertyId(pub u32);

/// Stable property ids used by generated profiles and golden tests.
pub mod properties {
    use super::PropertyId;

    pub const BATTERY: PropertyId = PropertyId(1);
    pub const VOLTAGE: PropertyId = PropertyId(2);
    pub const STATE: PropertyId = PropertyId(3);
    pub const BRIGHTNESS: PropertyId = PropertyId(4);
    pub const COLOR_TEMP: PropertyId = PropertyId(5);
    pub const COLOR: PropertyId = PropertyId(6);
    pub const COLOR_MODE: PropertyId = PropertyId(7);
    pub const TEMPERATURE: PropertyId = PropertyId(8);
    pub const HUMIDITY: PropertyId = PropertyId(9);
    pub const OCCUPANCY: PropertyId = PropertyId(10);
    pub const ILLUMINANCE: PropertyId = PropertyId(11);
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ColorValue {
    /// 0..1 coordinates; missing coordinates stay missing (partial frames).
    Xy { x: Option<f32>, y: Option<f32> },
    /// hue 0..360, saturation 0..100; missing values stay missing.
    Hs { hue: Option<f32>, saturation: Option<f32> },
}

impl ColorValue {
    pub const fn xy(x: f32, y: f32) -> Self {
        Self::Xy {
            x: Some(x),
            y: Some(y),
        }
    }

    pub const fn hs(hue: f32, saturation: f32) -> Self {
        Self::Hs {
            hue: Some(hue),
            saturation: Some(saturation),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value {
    Bool(bool),
    Int(i32),
    Float(f32),
    Uint(u32),
    Enum(u32),
    Color(ColorValue),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    Single,
    Double,
    Hold,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unit {
    None,
    Celsius,
    Percent,
    Mired,
    Volt,
    MilliVolt,
    Lux,
    Minutes,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AttrType {
    Bool,
    Uint8,
    Int8,
    Uint16,
    Int16,
    Uint32,
    Int32,
    Enum8,
    Float32,
}

/// Multiplier/divisor for raw integer values (`0.01 °C`, battery `0.5 %`, ...).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scale {
    pub mul: i32,
    pub div: i32,
}

impl Scale {
    pub const NONE: Self = Self { mul: 1, div: 1 };

    pub fn apply(self, raw: i32) -> f32 {
        if self.div == 0 {
            return raw as f32;
        }
        raw as f32 * self.mul as f32 / self.div as f32
    }
}

/// Same for both directions; used by `to: &[(u8, Action)]`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Manufacturer {
    Exact(&'static str),
    /// Spike helper for Tuya `_TZ3000_*` families; the generator emits exact
    /// names where Z2M does.
    Prefix(&'static str),
    Any,
}

pub struct Fingerprint {
    pub manufacturer: Manufacturer,
    pub model: &'static str,
}

pub enum Expose {
    Binary { property: PropertyId },
    Numeric {
        property: PropertyId,
        unit: Unit,
        min: f32,
        max: f32,
        step: f32,
    },
    Color { property: PropertyId },
    Enumeration {
        property: PropertyId,
        values: &'static [(u32, &'static str)],
    },
    Action(&'static str),
    Composite(&'static [Expose]),
}

pub struct AttributeRule {
    pub cluster: u16,
    pub attribute: u16,
    pub kind: AttrType,
    pub property: PropertyId,
    pub scale: Scale,
    /// Z2M often ignores a sentinel value (e.g. 255 = unknown for battery).
    pub skip: Option<i64>,
}

pub enum Converter {
    Attribute(AttributeRule),
    /// Cluster-specific command where one payload byte selects the action.
    Command {
        cluster: u16,
        command: u8,
        actions: &'static [(u8, Action)],
    },
    /// Non-standard payload framing inside a normal cluster (Tuya DP, Xiaomi
    /// special, ...). The codec itself is a portable function, not a subsystem.
    Codec {
        cluster: u16,
        decode: fn(&[u8], &mut Decoded) -> bool,
    },
    Custom(fn(&RawZcl<'_>, &mut Decoded) -> bool),
    /// Converter that needs the whole attribute frame (Z2M semantics).
    Frame(for<'a> fn(&AttributeFrameView<'a>, &mut Decoded) -> bool),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Update {
    Property { property: PropertyId, value: Value },
    Action(Action),
}

/// Fixed-capacity result of one event: no heap allocation on the hot path.
#[derive(Clone, Debug, PartialEq)]
pub struct Decoded {
    items: [Option<Update>; MAX_UPDATES],
    len: usize,
}

impl Default for Decoded {
    fn default() -> Self {
        Self {
            items: [None; MAX_UPDATES],
            len: 0,
        }
    }
}

impl Decoded {
    pub fn push(&mut self, update: Update) -> bool {
        if self.len == MAX_UPDATES {
            return false;
        }
        self.items[self.len] = Some(update);
        self.len += 1;
        true
    }

    pub fn updates(&self) -> impl Iterator<Item = &Update> {
        self.items.iter().flatten()
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

/// One raw ZCL event from the transport; borrowed for the duration of decoding.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RawZcl<'a> {
    pub device: DeviceKey,
    pub endpoint: u8,
    pub cluster: u16,
    pub command: u8,
    /// Attribute id for foundation Report/ReadResponse frames; None for commands.
    pub attribute: Option<u16>,
    pub payload: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Interview<'a> {
    pub manufacturer: Option<&'a str>,
    pub model: &'a str,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Command {
    Property { property: PropertyId, value: Value },
    Action(Action),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommandCodec {
    None,
    /// brightness 0..254 -> moveToLevelWithOnOff(level, transition)
    Level,
    /// color temperature in mireds -> moveToColorTemp(mireds, transition)
    ColorTemp,
    /// XY 0..1 -> moveToColor(x, y, transition, options)
    ColorXy,
    /// hue 0..360 / sat 0..100 -> moveToHueAndSaturation(hue, sat, ...)
    ColorHs,
    /// hue 0..360 / sat 0..100 -> enhancedMoveToHueAndSaturation(hue, sat, ...)
    ColorHsEnhanced,
}

pub struct CommandMap {
    pub command: Command,
    pub cluster: u16,
    pub zcl_command: u8,
    /// Fixed payload bytes for exact commands; empty for codecs that encode the
    /// command value at runtime.
    pub args: &'static [u8],
    pub codec: CommandCodec,
}

/// Target endpoint for a configure step: a fixed id or the device's first
/// application endpoint (resolved by the runner).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConfigureEndpoint {
    Id(u8),
    First,
}

pub enum ConfigureStep {
    Read {
        endpoint: ConfigureEndpoint,
        cluster: u16,
        attributes: &'static [u16],
    },
    Bind {
        endpoint: ConfigureEndpoint,
        cluster: u16,
    },
    Report {
        endpoint: ConfigureEndpoint,
        cluster: u16,
        attribute: u16,
        attribute_type: u8,
        min: u16,
        max: u16,
        change: u32,
    },
    Write {
        endpoint: ConfigureEndpoint,
        cluster: u16,
        attribute: u16,
        value: Value,
    },
}

/// Async configuration contract: requests are correlated by seq, bounded by
/// timeout and retries (executed by the HA Zigbee Layer, not by this crate).
/// `version` is persisted as the record's `configured_version` on success.
pub struct ConfigurePlan {
    pub steps: &'static [ConfigureStep],
    pub timeout_ms: u32,
    pub retries: u8,
    pub version: u16,
}

/// Attributes of ZHC `tuya.configureMagicPacket` (genBasic read).
pub static TUYA_MAGIC_ATTRIBUTES: &[u16] = &[0x0004, 0x0000, 0x0001, 0x0005, 0x0007, 0xFFFE];

/// ZHC `tuya.configureMagicPacket`: read Basic attributes to trigger Tuya
/// configuration. Z2M uses `device.endpoints[0]`, the first app endpoint.
pub static TUYA_MAGIC_PACKET: ConfigurePlan = ConfigurePlan {
    steps: &[ConfigureStep::Read {
        endpoint: ConfigureEndpoint::First,
        cluster: 0x0000,
        attributes: TUYA_MAGIC_ATTRIBUTES,
    }],
    timeout_ms: 30_000,
    retries: 3,
    version: 1,
};

/// Coverage bitmask: identified -> decoded -> controlled; partial helpers set only the covered bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Support(u8);

impl Support {
    pub const IDENTIFIED: Self = Self(1);
    pub const DECODED: Self = Self(2);
    pub const CONTROLLED: Self = Self(4);
    /// Some helper options/converter rules are not generated (reported explicitly).
    pub const PARTIAL: Self = Self(8);

    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }

    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}

pub struct Profile {
    pub id: &'static str,
    pub vendor: &'static str,
    pub fingerprints: &'static [Fingerprint],
    pub exposes: &'static [Expose],
    pub from: &'static [Converter],
    pub to: &'static [CommandMap],
    pub configure: Option<&'static ConfigurePlan>,
    /// Z2M `omitOptionalLevelAndColorParams`: omit optionsMask/optionsOverride.
    pub omit_optional_params: bool,
    /// Z2M turnsOffAtBrightness1: level 1 is sent as 2.
    pub turns_off_at_brightness_1: bool,
    pub support: Support,
}

pub static PROFILES: &[&Profile] = &[&TUYA_TS0041, &SNZB_01P, &TS011F];

pub fn identify(interview: &Interview<'_>) -> Option<&'static Profile> {
    let device = DeviceIdentity {
        model: interview.model,
        manufacturer_name: interview.manufacturer,
        manufacturer_id: None,
        type_: None,
        power_source: None,
        software_build_id: None,
        date_code: None,
        application_version: None,
        hardware_version: None,
        zcl_version: None,
        ieee: None,
        endpoints: &[],
    };
    match_definition(&device).and_then(|definition| definition.profile)
}
fn normalized_model(model: &str) -> &str {
    let end = model.find('\0').unwrap_or(model.len());
    model[..end].trim()
}

/// `fromZigbee`: raw ZCL -> normalized properties/actions.
pub fn decode(profile: &Profile, event: &RawZcl<'_>) -> Option<Decoded> {
    let mut decoded = Decoded::default();
    for converter in profile.from {
        match converter {
            Converter::Attribute(rule) => {
                if event.cluster == rule.cluster && event.attribute == Some(rule.attribute) {
                    if let Some((raw, value)) = read_raw(rule.kind, event.payload) {
                        if rule.skip == Some(raw) {
                            continue;
                        }
                        decoded.push(Update::Property {
                            property: rule.property,
                            value: scale_value(value, rule.scale),
                        });
                    }
                }
            }
            Converter::Command {
                cluster,
                command,
                actions,
            } => {
                if event.cluster == *cluster && event.command == *command {
                    let first = event.payload.first().copied();
                    if let Some(&(_, action)) = actions.iter().find(|&&(byte, _)| Some(byte) == first) {
                        decoded.push(Update::Action(action));
                    }
                }
            }
            Converter::Codec { cluster, decode } => {
                if event.cluster == *cluster {
                    let _ = decode(event.payload, &mut decoded);
                }
            }
            Converter::Custom(handler) => {
                let _ = handler(event, &mut decoded);
            }
            Converter::Frame(_) => {}
        }
    }
    (!decoded.is_empty()).then_some(decoded)
}

/// Attribute frame -> normalized updates (one ZCL frame may carry many attributes).
pub fn decode_frame(profile: &Profile, frame: &AttributeFrameView<'_>, out: &mut Decoded) -> usize {
    let before = out.len();
    for converter in profile.from {
        match converter {
            Converter::Frame(handler) => {
                let _ = handler(frame, out);
            }
            Converter::Attribute(rule) => {
                if frame.cluster != rule.cluster {
                    continue;
                }
                for attr in frame.attributes() {
                    if attr.id != rule.attribute {
                        continue;
                    }
                    if let Some((raw, value)) = read_raw(rule.kind, attr.value) {
                        if rule.skip == Some(raw) {
                            continue;
                        }
                        out.push(Update::Property {
                            property: rule.property,
                            value: scale_value(value, rule.scale),
                        });
                    }
                }
            }
            _ => {}
        }
    }
    out.len() - before
}

/// `toZigbee` for fixed-argument commands.
pub fn resolve_command(profile: &Profile, command: &Command) -> Option<(u16, u8)> {
    find_command(profile, command).map(|map| (map.cluster, map.zcl_command))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EncodeOptions {
    pub transition: u16,
    /// Z2M sends `optionsMask/optionsOverride` by default; `None` matches
    /// `omitOptionalLevelAndColorParams`.
    pub options: Option<(u8, u8)>,
}

impl Default for EncodeOptions {
    fn default() -> Self {
        Self {
            transition: 0,
            options: Some((0, 0)),
        }
    }
}

pub fn encode(profile: &Profile, command: &Command, out: &mut [u8]) -> Option<usize> {
    let options = if profile.omit_optional_params {
        EncodeOptions {
            transition: 0,
            options: None,
        }
    } else {
        EncodeOptions::default()
    };
    encode_with_options(profile, command, options, out)
}

pub fn encode_with_options(
    profile: &Profile,
    command: &Command,
    options: EncodeOptions,
    out: &mut [u8],
) -> Option<usize> {
    let map = find_command(profile, command)?;
    let transition = options.transition.to_le_bytes();
    let extra = if options.options.is_some() { 2 } else { 0 };
    match map.codec {
        CommandCodec::None => {
            if out.len() < map.args.len() {
                return None;
            }
            out[..map.args.len()].copy_from_slice(map.args);
            Some(map.args.len())
        }
        CommandCodec::Level => {
            let needed = 3 + extra;
            if out.len() < needed {
                return None;
            }
            let mut level = numeric_value(command)?.clamp(0.0, 254.0).round() as u8;
            if profile.turns_off_at_brightness_1 && level == 1 {
                level = 2;
            }
            out[0] = level;
            out[1..3].copy_from_slice(&transition);
            Some(with_options(out, 3, options.options))
        }
        CommandCodec::ColorTemp => {
            let needed = 4 + extra;
            if out.len() < needed {
                return None;
            }
            let mireds = numeric_value(command)?.clamp(0.0, 65535.0).round() as u16;
            out[..2].copy_from_slice(&mireds.to_le_bytes());
            out[2..4].copy_from_slice(&transition);
            Some(with_options(out, 4, options.options))
        }
        CommandCodec::ColorXy => {
            let ColorValue::Xy {
                x: Some(x),
                y: Some(y),
            } = color_value(command)?
            else {
                return None;
            };
            let needed = 6 + extra;
            if out.len() < needed {
                return None;
            }
            let x = (x.clamp(0.0, 1.0) * 65535.0).round() as u16;
            let y = (y.clamp(0.0, 1.0) * 65535.0).round() as u16;
            out[..2].copy_from_slice(&x.to_le_bytes());
            out[2..4].copy_from_slice(&y.to_le_bytes());
            out[4..6].copy_from_slice(&transition);
            Some(with_options(out, 6, options.options))
        }
        CommandCodec::ColorHs | CommandCodec::ColorHsEnhanced => {
            let ColorValue::Hs {
                hue: Some(hue),
                saturation: Some(saturation),
            } = color_value(command)?
            else {
                return None;
            };
            let hue = hue.clamp(0.0, 360.0);
            let saturation = saturation.clamp(0.0, 100.0);
            if map.codec == CommandCodec::ColorHsEnhanced {
                let needed = 5 + extra;
                if out.len() < needed {
                    return None;
                }
                let hue = (hue * 65535.0 / 360.0).round() as u16;
                let saturation = (saturation * 254.0 / 100.0).round() as u8;
                out[..2].copy_from_slice(&hue.to_le_bytes());
                out[2] = saturation;
                out[3..5].copy_from_slice(&transition);
                Some(with_options(out, 5, options.options))
            } else {
                let needed = 4 + extra;
                if out.len() < needed {
                    return None;
                }
                let hue = (hue * 254.0 / 360.0).round() as u8;
                let saturation = (saturation * 254.0 / 100.0).round() as u8;
                out[0] = hue;
                out[1] = saturation;
                out[2..4].copy_from_slice(&transition);
                Some(with_options(out, 4, options.options))
            }
        }
    }
}

fn with_options(out: &mut [u8], len: usize, options: Option<(u8, u8)>) -> usize {
    match options {
        Some((mask, overrides)) => {
            out[len] = mask;
            out[len + 1] = overrides;
            len + 2
        }
        None => len,
    }
}
fn color_value(command: &Command) -> Option<ColorValue> {
    match command {
        Command::Property {
            value: Value::Color(color),
            ..
        } => Some(*color),
        _ => None,
    }
}

fn numeric_value(command: &Command) -> Option<f32> {
    let Command::Property { value, .. } = command else {
        return None;
    };
    Some(match value {
        Value::Int(v) => *v as f32,
        Value::Float(v) => *v,
        Value::Uint(v) => *v as f32,
        Value::Enum(v) => *v as f32,
        Value::Bool(v) => u8::from(*v) as f32,
        Value::Color(_) => return None,
    })
}

fn find_command<'a>(profile: &'a Profile, command: &Command) -> Option<&'a CommandMap> {
    profile.to.iter().find(|map| match map.codec {
        CommandCodec::None => match (command, &map.command) {
            (
                Command::Property {
                    property: wanted,
                    value: wanted_value,
                },
                Command::Property {
                    property: mapped,
                    value: mapped_value,
                },
            ) => wanted == mapped && wanted_value == mapped_value,
            (Command::Action(action), Command::Action(mapped)) => action == mapped,
            _ => false,
        },
        CommandCodec::Level | CommandCodec::ColorTemp => matches!(
            (command, &map.command),
            (
                Command::Property { property, .. },
                Command::Property { property: mapped, .. }
            ) if property == mapped
        ),
        CommandCodec::ColorXy => matches!(
            (command, &map.command),
            (
                Command::Property {
                    property,
                    value: Value::Color(ColorValue::Xy { .. }),
                },
                Command::Property { property: mapped, .. }
            ) if property == mapped
        ),
        CommandCodec::ColorHs | CommandCodec::ColorHsEnhanced => matches!(
            (command, &map.command),
            (
                Command::Property {
                    property,
                    value: Value::Color(ColorValue::Hs { .. }),
                },
                Command::Property { property: mapped, .. }
            ) if property == mapped
        ),
    })
}

fn read_raw(kind: AttrType, payload: &[u8]) -> Option<(i64, Value)> {
    Some(match kind {
        AttrType::Bool => {
            let raw = *payload.first()?;
            (i64::from(raw), Value::Bool(raw == 1))
        }
        AttrType::Uint8 | AttrType::Enum8 => {
            let raw = *payload.first()?;
            (i64::from(raw), Value::Int(i32::from(raw)))
        }
        AttrType::Int8 => {
            let raw = *payload.first()? as i8;
            (i64::from(raw), Value::Int(i32::from(raw)))
        }
        AttrType::Uint16 => {
            let raw = u16::from_le_bytes(words(payload)?);
            (i64::from(raw), Value::Int(i32::from(raw)))
        }
        AttrType::Int16 => {
            let raw = i16::from_le_bytes(words(payload)?);
            (i64::from(raw), Value::Int(i32::from(raw)))
        }
        AttrType::Uint32 => {
            let raw = u32::from_le_bytes(quads(payload)?);
            (i64::from(raw), Value::Uint(raw))
        }
        AttrType::Int32 => {
            let raw = i32::from_le_bytes(quads(payload)?);
            (i64::from(raw), Value::Int(raw))
        }
        AttrType::Float32 => (0, Value::Float(f32::from_le_bytes(quads(payload)?))),
    })
}

fn words(payload: &[u8]) -> Option<[u8; 2]> {
    Some([*payload.first()?, *payload.get(1)?])
}

fn quads(payload: &[u8]) -> Option<[u8; 4]> {
    Some([
        *payload.first()?,
        *payload.get(1)?,
        *payload.get(2)?,
        *payload.get(3)?,
    ])
}

fn scale_value(value: Value, scale: Scale) -> Value {
    if scale == Scale::NONE {
        return value;
    }
    match value {
        Value::Int(raw) => Value::Float(scale.apply(raw)),
        other => other,
    }
}

/// First profile: Tuya TS0041 single-button remote (OnOff 0xFD, payload 00/01/02).
pub static TUYA_TS0041: Profile = Profile {
    id: "tuya.ts0041",
    vendor: "Tuya",
    fingerprints: &[Fingerprint {
        manufacturer: Manufacturer::Prefix("_TZ3000_"),
        model: "TS0041",
    }],
    exposes: &[
        Expose::Numeric {
            property: crate::properties::BATTERY,
            unit: Unit::Percent,
            min: 0.0,
            max: 100.0,
            step: 1.0,
        },
        Expose::Action("single"),
        Expose::Action("double"),
        Expose::Action("hold"),
    ],
    // ZHC: tuya.configureMagicPacket; no Bind/Reporting for this model.
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED
        .union(Support::DECODED)
        .union(Support::PARTIAL),
    from: &[
        Converter::Command {
            cluster: 0x0006,
            command: 0xFD,
            actions: &[
                (0x00, Action::Single),
                (0x01, Action::Double),
                (0x02, Action::Hold),
            ],
        },
        // ZHC: fz.battery -> batteryPercentageRemaining / 2, 255 = unknown.
        Converter::Attribute(AttributeRule {
            cluster: 0x0001,
            attribute: 0x0021,
            kind: AttrType::Uint8,
            property: crate::properties::BATTERY,
            scale: Scale { mul: 1, div: 2 },
            skip: Some(255),
        }),
    ],
    to: &[],
    configure: Some(&TUYA_MAGIC_PACKET),
};

#[cfg(test)]
mod tests {
    use super::*;

    fn event<'a>(cluster: u16, command: u8, payload: &'a [u8]) -> RawZcl<'a> {
        RawZcl {
            device: [1; 8],
            endpoint: 1,
            cluster,
            command,
            attribute: None,
            payload,
        }
    }

    #[test]
    fn identify_mirrors_zhc_model_fallback() {
        for manufacturer in [Some("_TZ3000_abcd"), Some("Other"), None] {
            let profile = identify(&Interview {
                manufacturer,
                model: "TS0041",
            })
            .unwrap();
            assert_eq!(profile.id, "tuya.ts0041");
        }
        assert!(identify(&Interview {
            manufacturer: None,
            model: "UNKNOWN-MODEL",
        })
        .is_none());
    }

    #[test]
    fn ts0041_payloads_decode_to_actions() {
        for (byte, action) in [
            (0x00u8, Action::Single),
            (0x01, Action::Double),
            (0x02, Action::Hold),
        ] {
            let decoded = decode(&TUYA_TS0041, &event(0x0006, 0xFD, &[byte])).unwrap();
            assert_eq!(decoded.updates().copied().collect::<Vec<_>>(), vec![Update::Action(action)]);
        }
    }

    #[test]
    fn foreign_frames_are_not_claimed() {
        assert!(decode(&TUYA_TS0041, &event(0x0006, 0x01, &[0x00])).is_none());
        assert!(decode(&TUYA_TS0041, &event(0x0001, 0xFD, &[0x00])).is_none());
        assert!(decode(&TUYA_TS0041, &event(0x0006, 0xFD, &[])).is_none());
        assert!(decode(&TUYA_TS0041, &event(0x0006, 0xFD, &[0x03])).is_none());
        // Z2M converters read the first payload byte and ignore the tail.
        assert!(decode(&TUYA_TS0041, &event(0x0006, 0xFD, &[0x00, 0x01])).is_some());
    }

    #[test]
    fn attribute_rules_decode_and_scale() {
        static FROM: &[Converter] = &[Converter::Attribute(AttributeRule {
            cluster: 0x0402,
            attribute: 0x0000,
            kind: AttrType::Int16,
            property: PropertyId(7),
            scale: Scale { mul: 1, div: 100 },
            skip: None,
        })];
        static PROFILE: Profile = Profile {
            id: "test.sensor",
            vendor: "Test",
            fingerprints: &[],
            exposes: &[],
            from: FROM,
            to: &[],
            configure: None,
            omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
        };
        let bytes = (-512i16).to_le_bytes();
        let mut ev = event(0x0402, 0x0A, &bytes);
        ev.attribute = Some(0x0000);
        let decoded = decode(&PROFILE, &ev).unwrap();
        assert_eq!(
            decoded.updates().copied().collect::<Vec<_>>(),
            vec![Update::Property {
                property: PropertyId(7),
                value: Value::Float(-5.12)
            }]
        );
    }

    #[test]
    fn to_zigbee_encodes_fixed_commands() {
        static TO: &[CommandMap] = &[CommandMap {
            command: Command::Property {
                property: PropertyId(1),
                value: Value::Bool(true),
            },
            cluster: 0x0006,
            zcl_command: 0x01,
            args: &[],
            codec: CommandCodec::None,
        }];
        static PROFILE: Profile = Profile {
            id: "test.onoff",
            vendor: "Test",
            fingerprints: &[],
            exposes: &[],
            from: &[],
            to: TO,
            configure: None,
            omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
        };
        let mut out = [0u8; 8];
        let len = encode(
            &PROFILE,
            &Command::Property {
                property: PropertyId(1),
                value: Value::Bool(true),
            },
            &mut out,
        );
        assert_eq!(len, Some(0));
        assert_eq!(
            encode(
                &PROFILE,
                &Command::Property {
                    property: PropertyId(1),
                    value: Value::Bool(false)
                },
                &mut out
            ),
            None
        );
    }

    #[test]
    fn decoded_buffer_is_bounded() {
        let mut decoded = Decoded::default();
        for _ in 0..MAX_UPDATES {
            assert!(decoded.push(Update::Action(Action::Single)));
        }
        assert!(!decoded.push(Update::Action(Action::Single)));
        assert_eq!(decoded.len(), MAX_UPDATES);
    }

    #[test]
    fn frame_with_several_attributes_decodes_all_matching_rules() {
        let payload = [0x21, 0x00, 0x20, 200, 0x20, 0x00, 0x20, 30];
        let frame = AttributeFrameView::parse(0x0001, 1, 0x0A, &payload).unwrap();
        let mut decoded = Decoded::default();
        assert_eq!(decode_frame(&SNZB_01P, &frame, &mut decoded), 2);
        let updates: Vec<_> = decoded.updates().copied().collect();
        assert_eq!(
            updates[0],
            Update::Property {
                property: properties::BATTERY,
                value: Value::Float(100.0)
            }
        );
        assert_eq!(
            updates[1],
            Update::Property {
                property: properties::VOLTAGE,
                value: Value::Float(3000.0)
            }
        );
    }

    #[test]
    fn frame_with_unknown_attribute_ids_yields_no_updates() {
        let payload = [0x55, 0x00, 0x20, 1];
        let frame = AttributeFrameView::parse(0x0001, 1, 0x0A, &payload).unwrap();
        let mut decoded = Decoded::default();
        assert_eq!(decode_frame(&SNZB_01P, &frame, &mut decoded), 0);
        assert!(decoded.is_empty());
    }
}

#[cfg(test)]
mod ts0041_battery_tests {
    use super::*;

    fn frame(payload: &[u8]) -> AttributeFrameView<'_> {
        AttributeFrameView::parse(0x0001, 1, 0x0A, payload).unwrap()
    }

    #[test]
    fn battery_percentage_matches_z2m() {
        for (raw, expected) in [(200u8, 100.0f32), (123, 61.5)] {
            let mut out = Decoded::default();
            assert_eq!(
                decode_frame(&TUYA_TS0041, &frame(&[0x21, 0x00, 0x20, raw]), &mut out),
                1
            );
            let update = out.updates().next().unwrap();
            assert_eq!(
                *update,
                Update::Property {
                    property: properties::BATTERY,
                    value: Value::Float(expected)
                }
            );
        }
    }

    #[test]
    fn unknown_battery_value_and_foreign_attribute_are_ignored() {
        let mut out = Decoded::default();
        assert_eq!(decode_frame(&TUYA_TS0041, &frame(&[0x21, 0x00, 0x20, 255]), &mut out), 0);
        assert_eq!(decode_frame(&TUYA_TS0041, &frame(&[0x55, 0x00, 0x20, 1]), &mut out), 0);
    }
    #[test]
    fn ts0041_configure_is_tuya_magic_packet() {
        let plan = TUYA_TS0041.configure.expect("configure plan");
        assert_eq!(plan.version, 1);
        assert_eq!(plan.steps.len(), 1);
        assert!(matches!(
            plan.steps[0],
            ConfigureStep::Read {
                endpoint: ConfigureEndpoint::First,
                cluster: 0x0000,
                attributes,
            } if attributes == TUYA_MAGIC_ATTRIBUTES
        ));
    }
}