//! Bounded lifecycle of active HA commands. This is not a second state store:
//! `Desired` lives only while a command is active; `Reported` is Domain state.

use micro_db::Error;

use ha_domain::canonical::{
    CanonicalCommand, CommandValue, EntityId, PropertyId, ACTION_OFF, ACTION_ON, ACTION_SET,
    ACTION_TOGGLE, PROPERTY_BRIGHTNESS, PROPERTY_COLOR, PROPERTY_COLOR_TEMPERATURE,
    PROPERTY_COLOR_X, PROPERTY_COLOR_Y, PROPERTY_POWER,
};
use ha_domain::{Value, ValueType};

pub type OperationToken = u64;
pub const PENDING_MAX: usize = 8;
const REPORTED_MAX: usize = 16;
const SCALAR_EPSILON: f32 = 0.01;
const COLOR_XY_EPSILON: f32 = 0.001;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommandStatus {
    Sent,
    Acknowledged,
    Failed,
    Timeout,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Expected {
    Value {
        property: PropertyId,
        value: Value,
    },
    Color {
        x: f32,
        y: f32,
        seen_x: bool,
        seen_y: bool,
    },
    Toggle {
        expected: Option<bool>,
    },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PendingCommand {
    pub entity: EntityId,
    pub property: PropertyId,
    pub expected: Expected,
    pub operation: OperationToken,
    pub status: CommandStatus,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Reported {
    entity: EntityId,
    property: PropertyId,
    value: Value,
}

pub trait CommandSink {
    fn send(&mut self, command: &CanonicalCommand, now: u64) -> Result<OperationToken, Error>;
}

pub struct CommandCore {
    pending: [Option<PendingCommand>; PENDING_MAX],
    reported: [Option<Reported>; REPORTED_MAX],
}

impl Default for CommandCore {
    fn default() -> Self {
        Self::new()
    }
}

impl CommandCore {
    pub fn new() -> Self {
        Self {
            pending: [None; PENDING_MAX],
            reported: [None; REPORTED_MAX],
        }
    }

    pub fn submit<S: CommandSink>(
        &mut self,
        sink: &mut S,
        command: CanonicalCommand,
        now: u64,
    ) -> Result<OperationToken, Error> {
        let expected =
            expected_for(&command, self.last_power(command.entity)).ok_or(Error::InvalidArg)?;
        let slot = self
            .pending
            .iter()
            .position(|entry| {
                entry.is_some_and(|entry| {
                    entry.entity == command.entity && entry.property == command.property
                })
            })
            .or_else(|| self.pending.iter().position(Option::is_none))
            .ok_or(Error::NoSpace)?;
        let operation = sink.send(&command, now)?;
        self.pending[slot] = Some(PendingCommand {
            entity: command.entity,
            property: command.property,
            expected,
            operation,
            status: CommandStatus::Sent,
        });
        Ok(operation)
    }

    /// Confirmed/acknowledged transport outcomes are not reported state.
    /// Final failures free the slot; success stays until a matching report.
    pub fn on_result(&mut self, operation: OperationToken, status: CommandStatus) -> bool {
        let Some(slot) = self
            .pending
            .iter()
            .position(|entry| entry.is_some_and(|entry| entry.operation == operation))
        else {
            return false;
        };
        match status {
            CommandStatus::Failed | CommandStatus::Timeout => {
                self.pending[slot] = None;
                false
            }
            CommandStatus::Sent | CommandStatus::Acknowledged => {
                if let Some(entry) = self.pending[slot].as_mut() {
                    entry.status = status;
                }
                true
            }
        }
    }

    /// A device report satisfies a command only when its value matches the
    /// expected state. Color needs both axes; toggle needs a known state.
    pub fn on_reported(&mut self, entity: EntityId, property: PropertyId, value: Value) {
        self.remember_reported(entity, property, value);
        for slot in &mut self.pending {
            let clear = if let Some(entry) = slot.as_mut() {
                entry.entity == entity
                    && match &mut entry.expected {
                        Expected::Value {
                            property: expected_property,
                            value: expected_value,
                        } => {
                            *expected_property == property && value_matches(expected_value, &value)
                        }
                        Expected::Color {
                            x,
                            y,
                            seen_x,
                            seen_y,
                        } => {
                            let Some(reported) = value.as_f32() else {
                                continue;
                            };
                            match property {
                                PROPERTY_COLOR_X => {
                                    *seen_x = (reported - *x).abs() <= COLOR_XY_EPSILON;
                                }
                                PROPERTY_COLOR_Y => {
                                    *seen_y = (reported - *y).abs() <= COLOR_XY_EPSILON;
                                }
                                _ => {}
                            }
                            *seen_x && *seen_y
                        }
                        Expected::Toggle { expected } => {
                            property == PROPERTY_POWER
                                && expected
                                    .is_some_and(|expected| value.as_bool() == Some(expected))
                        }
                    }
            } else {
                false
            };
            if clear {
                *slot = None;
            }
        }
    }

    pub fn pending(&self, operation: OperationToken) -> Option<&PendingCommand> {
        self.pending
            .iter()
            .flatten()
            .find(|entry| entry.operation == operation)
    }

    pub fn len(&self) -> usize {
        self.pending.iter().flatten().count()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn last_power(&self, entity: EntityId) -> Option<bool> {
        self.reported
            .iter()
            .flatten()
            .find(|entry| entry.entity == entity && entry.property == PROPERTY_POWER)
            .and_then(|entry| entry.value.as_bool())
    }

    fn remember_reported(&mut self, entity: EntityId, property: PropertyId, value: Value) {
        if let Some(entry) = self
            .reported
            .iter_mut()
            .flatten()
            .find(|entry| entry.entity == entity && entry.property == property)
        {
            entry.value = value;
            return;
        }
        if let Some(slot) = self.reported.iter_mut().find(|entry| entry.is_none()) {
            *slot = Some(Reported {
                entity,
                property,
                value,
            });
        }
    }
}

fn expected_for(command: &CanonicalCommand, last_power: Option<bool>) -> Option<Expected> {
    match command.property {
        PROPERTY_POWER => match command.action {
            ACTION_ON => Some(Expected::Value {
                property: PROPERTY_POWER,
                value: Value::bool(true),
            }),
            ACTION_OFF => Some(Expected::Value {
                property: PROPERTY_POWER,
                value: Value::bool(false),
            }),
            ACTION_TOGGLE => Some(Expected::Toggle {
                expected: last_power.map(|value| !value),
            }),
            _ => None,
        },
        PROPERTY_BRIGHTNESS | PROPERTY_COLOR_TEMPERATURE if command.action == ACTION_SET => {
            let CommandValue::Scalar(value) = command.value else {
                return None;
            };
            Some(Expected::Value {
                property: command.property,
                value,
            })
        }
        PROPERTY_COLOR if command.action == ACTION_SET => {
            let CommandValue::Xy { x, y } = command.value else {
                return None;
            };
            Some(Expected::Color {
                x,
                y,
                seen_x: false,
                seen_y: false,
            })
        }
        _ => None,
    }
}

fn value_matches(expected: &Value, reported: &Value) -> bool {
    if expected.kind != reported.kind {
        return false;
    }
    if expected.kind == ValueType::F32 {
        return match (expected.as_f32(), reported.as_f32()) {
            (Some(expected), Some(reported)) => (expected - reported).abs() <= SCALAR_EPSILON,
            _ => false,
        };
    }
    expected == reported
}
#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::canonical::{
        ACTION_ON, ACTION_SET, ACTION_TOGGLE, PROPERTY_BRIGHTNESS, PROPERTY_COLOR,
        PROPERTY_COLOR_X, PROPERTY_COLOR_Y, PROPERTY_POWER,
    };

    struct FakeSink {
        next: OperationToken,
    }

    impl CommandSink for FakeSink {
        fn send(
            &mut self,
            _command: &CanonicalCommand,
            _now: u64,
        ) -> Result<OperationToken, Error> {
            self.next += 1;
            Ok(self.next)
        }
    }

    fn command(property: PropertyId, action: u32, value: CommandValue) -> CanonicalCommand {
        CanonicalCommand {
            entity: 9,
            property,
            action,
            value,
        }
    }

    #[test]
    fn matching_report_clears_desired_and_ack_does_not() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        let op = core
            .submit(
                &mut sink,
                command(PROPERTY_POWER, ACTION_ON, CommandValue::None),
                0,
            )
            .unwrap();
        assert_eq!(core.len(), 1);
        assert!(core.on_result(op, CommandStatus::Acknowledged));
        assert_eq!(
            core.pending(op).unwrap().status,
            CommandStatus::Acknowledged
        );
        assert_eq!(
            core.pending(op).unwrap().expected,
            Expected::Value {
                property: PROPERTY_POWER,
                value: Value::bool(true)
            }
        );

        core.on_reported(9, PROPERTY_POWER, Value::bool(false));
        assert_eq!(core.len(), 1);
        core.on_reported(9, PROPERTY_POWER, Value::bool(true));
        assert!(core.is_empty());
    }

    #[test]
    fn mismatched_scalar_does_not_clear_and_matching_tolerance_does() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        core.submit(
            &mut sink,
            command(
                PROPERTY_BRIGHTNESS,
                ACTION_SET,
                CommandValue::Scalar(Value::f32(50.0)),
            ),
            0,
        )
        .unwrap();

        core.on_reported(9, PROPERTY_BRIGHTNESS, Value::f32(49.0));
        assert_eq!(core.len(), 1);
        core.on_reported(9, PROPERTY_BRIGHTNESS, Value::f32(50.005));
        assert!(core.is_empty());
    }

    #[test]
    fn unrelated_report_does_not_clear() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        core.submit(
            &mut sink,
            command(PROPERTY_POWER, ACTION_ON, CommandValue::None),
            0,
        )
        .unwrap();

        core.on_reported(9, PROPERTY_BRIGHTNESS, Value::f32(50.0));
        assert_eq!(core.len(), 1);
    }

    #[test]
    fn color_needs_both_axes_and_both_values() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        core.submit(
            &mut sink,
            command(
                PROPERTY_COLOR,
                ACTION_SET,
                CommandValue::Xy { x: 0.5, y: 0.25 },
            ),
            0,
        )
        .unwrap();

        core.on_reported(9, PROPERTY_COLOR_X, Value::f32(0.5004));
        assert_eq!(core.len(), 1);
        core.on_reported(9, PROPERTY_COLOR_Y, Value::f32(0.2504));
        assert!(core.is_empty());
    }

    #[test]
    fn color_mismatch_resets_axis_progress() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        core.submit(
            &mut sink,
            command(
                PROPERTY_COLOR,
                ACTION_SET,
                CommandValue::Xy { x: 0.5, y: 0.25 },
            ),
            0,
        )
        .unwrap();

        core.on_reported(9, PROPERTY_COLOR_X, Value::f32(0.5));
        core.on_reported(9, PROPERTY_COLOR_Y, Value::f32(0.9));
        assert_eq!(core.len(), 1);
        core.on_reported(9, PROPERTY_COLOR_X, Value::f32(0.9));
        core.on_reported(9, PROPERTY_COLOR_Y, Value::f32(0.25));
        assert_eq!(core.len(), 1);
        core.on_reported(9, PROPERTY_COLOR_X, Value::f32(0.5));
        assert!(core.is_empty());
    }

    #[test]
    fn toggle_uses_known_state() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        core.on_reported(9, PROPERTY_POWER, Value::bool(false));
        core.submit(
            &mut sink,
            command(PROPERTY_POWER, ACTION_TOGGLE, CommandValue::None),
            0,
        )
        .unwrap();

        core.on_reported(9, PROPERTY_POWER, Value::bool(false));
        assert_eq!(core.len(), 1);
        core.on_reported(9, PROPERTY_POWER, Value::bool(true));
        assert!(core.is_empty());
    }

    #[test]
    fn toggle_without_known_state_waits_for_timeout() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        let op = core
            .submit(
                &mut sink,
                command(PROPERTY_POWER, ACTION_TOGGLE, CommandValue::None),
                0,
            )
            .unwrap();

        core.on_reported(9, PROPERTY_POWER, Value::bool(false));
        core.on_reported(9, PROPERTY_POWER, Value::bool(true));
        assert_eq!(core.len(), 1);
        assert!(!core.on_result(op, CommandStatus::Timeout));
        assert!(core.is_empty());
    }

    #[test]
    fn timeout_frees_slot_without_report() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        let op = core
            .submit(
                &mut sink,
                command(
                    PROPERTY_BRIGHTNESS,
                    ACTION_SET,
                    CommandValue::Scalar(Value::f32(50.0)),
                ),
                0,
            )
            .unwrap();
        assert!(!core.on_result(op, CommandStatus::Timeout));
        assert!(core.is_empty());
    }

    #[test]
    fn pending_is_bounded() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        for index in 0..PENDING_MAX {
            let mut command = command(PROPERTY_POWER, ACTION_ON, CommandValue::None);
            command.entity = 10 + index as u64;
            core.submit(&mut sink, command, 0).unwrap();
        }
        assert_eq!(
            core.submit(
                &mut sink,
                command(PROPERTY_POWER, ACTION_ON, CommandValue::None),
                0
            ),
            Err(Error::NoSpace)
        );
    }

    #[test]
    fn same_entity_property_replaces_pending_command() {
        let mut sink = FakeSink { next: 0 };
        let mut core = CommandCore::new();
        for _ in 0..(PENDING_MAX * 2) {
            core.submit(
                &mut sink,
                command(PROPERTY_POWER, ACTION_ON, CommandValue::None),
                0,
            )
            .unwrap();
        }
        assert_eq!(core.len(), 1);
    }
}
