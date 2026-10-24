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

pub const MAX_UPDATES: usize = 8;

pub type DeviceKey = [u8; 8];

/// Opaque property id in the z2m-rs namespace (generator-assigned, stable).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PropertyId(pub u32);

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value {
    Bool(bool),
    Int(i32),
    Float(f32),
    Uint(u32),
    Enum(u32),
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
    Volt,
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

pub struct CommandMap {
    pub command: Command,
    pub cluster: u16,
    pub zcl_command: u8,
    /// Fixed payload bytes for Phase A; variable encoders arrive with the first
    /// device whose `toZigbee` needs them.
    pub args: &'static [u8],
}

pub enum ConfigureStep {
    Read {
        endpoint: u8,
        cluster: u16,
        attribute: u16,
    },
    Bind {
        endpoint: u8,
        cluster: u16,
    },
    Report {
        endpoint: u8,
        cluster: u16,
        attribute: u16,
        min: u16,
        max: u16,
        change: u32,
    },
    Write {
        endpoint: u8,
        cluster: u16,
        attribute: u16,
        value: Value,
    },
}

/// Async configuration contract: requests are correlated by seq, bounded by
/// timeout and retries (executed by the HA Zigbee Layer, not by this crate).
pub struct ConfigurePlan {
    pub steps: &'static [ConfigureStep],
    pub timeout_ms: u32,
    pub retries: u8,
}

pub struct Profile {
    pub id: &'static str,
    pub vendor: &'static str,
    pub fingerprints: &'static [Fingerprint],
    pub exposes: &'static [Expose],
    pub from: &'static [Converter],
    pub to: &'static [CommandMap],
    pub configure: Option<&'static ConfigurePlan>,
}

pub static PROFILES: &[&Profile] = &[&TUYA_TS0041];

pub fn identify(interview: &Interview<'_>) -> Option<&'static Profile> {
    PROFILES.iter().copied().find(|profile| {
        profile.fingerprints.iter().any(|fp| {
            fp.model == interview.model
                && match fp.manufacturer {
                    Manufacturer::Exact(name) => interview.manufacturer == Some(name),
                    Manufacturer::Prefix(prefix) => interview
                        .manufacturer
                        .is_some_and(|name| name.starts_with(prefix)),
                    Manufacturer::Any => true,
                }
        })
    })
}

/// `fromZigbee`: raw ZCL -> normalized properties/actions.
pub fn decode(profile: &Profile, event: &RawZcl<'_>) -> Option<Decoded> {
    let mut decoded = Decoded::default();
    for converter in profile.from {
        match converter {
            Converter::Attribute(rule) => {
                if event.cluster == rule.cluster {
                    if let Some(value) = read_value(rule.kind, event.payload) {
                        let value = scale_value(value, rule.scale);
                        decoded.push(Update::Property {
                            property: rule.property,
                            value,
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
        }
    }
    (!decoded.is_empty()).then_some(decoded)
}

/// `toZigbee` for fixed-argument commands.
pub fn encode(profile: &Profile, command: &Command, out: &mut [u8]) -> Option<usize> {
    let map = profile.to.iter().find(|map| match (command, &map.command) {
        (
            Command::Property { property, value },
            Command::Property {
                property: mapped,
                value: mapped_value,
            },
        ) => property == mapped && value == mapped_value,
        (Command::Action(action), Command::Action(mapped)) => action == mapped,
        _ => false,
    })?;
    if out.len() < map.args.len() {
        return None;
    }
    out[..map.args.len()].copy_from_slice(map.args);
    Some(map.args.len())
}

fn read_value(kind: AttrType, payload: &[u8]) -> Option<Value> {
    Some(match kind {
        AttrType::Bool => Value::Bool(*payload.first()? != 0),
        AttrType::Uint8 | AttrType::Enum8 => Value::Int(i32::from(*payload.first()?)),
        AttrType::Int8 => Value::Int(i32::from(*payload.first()? as i8)),
        AttrType::Uint16 => Value::Int(i32::from(u16::from_le_bytes(words(payload)?))),
        AttrType::Int16 => Value::Int(i32::from(i16::from_le_bytes(words(payload)?))),
        AttrType::Uint32 => Value::Uint(u32::from_le_bytes(quads(payload)?)),
        AttrType::Int32 => Value::Int(i32::from_le_bytes(quads(payload)?)),
        AttrType::Float32 => Value::Float(f32::from_le_bytes(quads(payload)?)),
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
        Expose::Action("single"),
        Expose::Action("double"),
        Expose::Action("hold"),
    ],
    from: &[Converter::Command {
        cluster: 0x0006,
        command: 0xFD,
        actions: &[
            (0x00, Action::Single),
            (0x01, Action::Double),
            (0x02, Action::Hold),
        ],
    }],
    to: &[],
    configure: None,
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
            payload,
        }
    }

    #[test]
    fn fingerprints_match_model_and_manufacturer() {
        let profile = identify(&Interview {
            manufacturer: Some("_TZ3000_abcd"),
            model: "TS0041",
        })
        .unwrap();
        assert_eq!(profile.id, "tuya.ts0041");
        assert!(identify(&Interview {
            manufacturer: Some("Other"),
            model: "TS0041"
        })
        .is_none());
        assert!(identify(&Interview {
            manufacturer: None,
            model: "TS0041"
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
        })];
        static PROFILE: Profile = Profile {
            id: "test.sensor",
            vendor: "Test",
            fingerprints: &[],
            exposes: &[],
            from: FROM,
            to: &[],
            configure: None,
        };
        let decoded = decode(&PROFILE, &event(0x0402, 0x0A, &(-512i16).to_le_bytes())).unwrap();
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
        }];
        static PROFILE: Profile = Profile {
            id: "test.onoff",
            vendor: "Test",
            fingerprints: &[],
            exposes: &[],
            from: &[],
            to: TO,
            configure: None,
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
}
