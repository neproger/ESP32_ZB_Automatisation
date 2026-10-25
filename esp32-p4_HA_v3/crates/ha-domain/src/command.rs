//! Bounded lifecycle of active HA commands. This is not a second state store:
//! `Desired` lives only while a command is active; `Reported` is Domain state.

use micro_db::Error;

use crate::canonical::{CanonicalCommand, CommandValue, EntityId, PropertyId};

pub type OperationToken = u64;
pub const PENDING_MAX: usize = 8;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommandStatus {
    Sent,
    Acknowledged,
    Failed,
    Timeout,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PendingCommand {
    pub entity: EntityId,
    pub property: PropertyId,
    pub desired: CommandValue,
    pub operation: OperationToken,
    pub status: CommandStatus,
}

pub trait CommandSink {
    fn send(&mut self, command: &CanonicalCommand, now: u64) -> Result<OperationToken, Error>;
}

pub struct CommandCore {
    pending: [Option<PendingCommand>; PENDING_MAX],
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
        }
    }

    pub fn submit<S: CommandSink>(
        &mut self,
        sink: &mut S,
        command: CanonicalCommand,
        now: u64,
    ) -> Result<OperationToken, Error> {
        let slot = self
            .pending
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let operation = sink.send(&command, now)?;
        self.pending[slot] = Some(PendingCommand {
            entity: command.entity,
            property: command.property,
            desired: command.value,
            operation,
            status: CommandStatus::Sent,
        });
        Ok(operation)
    }

    /// Confirmed/acknowledged transport outcomes are not reported state.
    /// Final failures free the slot; success stays until a report or clear.
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

    /// A device report satisfies the desired value of matching commands.
    pub fn on_reported(&mut self, entity: EntityId, property: PropertyId) {
        for slot in &mut self.pending {
            if slot.is_some_and(|entry| entry.entity == entity && entry.property == property) {
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
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::canonical::{
        ACTION_ON, ACTION_SET, PROPERTY_BRIGHTNESS, PROPERTY_POWER,
    };
    use crate::Value;

    struct FakeSink {
        next: OperationToken,
    }

    impl CommandSink for FakeSink {
        fn send(&mut self, _command: &CanonicalCommand, _now: u64) -> Result<OperationToken, Error> {
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
    fn desired_is_separate_from_reported_and_ack_does_not_report() {
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
        assert_eq!(core.pending(op).unwrap().status, CommandStatus::Acknowledged);
        assert_eq!(core.pending(op).unwrap().desired, CommandValue::None);
        // A report is the only thing that clears the desired command here.
        core.on_reported(9, PROPERTY_POWER);
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
        for _ in 0..PENDING_MAX {
            core.submit(
                &mut sink,
                command(PROPERTY_POWER, ACTION_ON, CommandValue::None),
                0,
            )
            .unwrap();
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
}
