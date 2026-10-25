//! Bridge between HA Command Core and the universal request executor.

use ha_domain::canonical::{CanonicalCommand, CommandValue, EntityId, PropertyId, ACTION_OFF, ACTION_ON, ACTION_SET, PROPERTY_POWER};
use ha_domain::command::{CommandCore, CommandSink, CommandStatus, OperationToken};
use ha_domain::Error;
use zigbee_exec::{
    ExecError, Executor, Expect, Payload, Request, RequestKind, RetryPolicy, Status,
};

use crate::canonical::{canonical_command, profile_capabilities};
use crate::descriptor::EndpointDescriptor;
use crate::standard;
use z2m_rs::Profile;

pub struct ZigbeeSink<'a> {
    pub executor: &'a mut Executor,
    pub profile: &'static Profile,
    pub device: (u64, u16),
    pub endpoint: u8,
}

impl CommandSink for ZigbeeSink<'_> {
    fn send(&mut self, command: &CanonicalCommand, now: u64) -> Result<OperationToken, Error> {
        let mapped = canonical_command(self.profile, command).ok_or(Error::InvalidArg)?;
        let (cluster, zcl_command) =
            z2m_rs::resolve_command(self.profile, &mapped).ok_or(Error::InvalidArg)?;
        let mut bytes = [0u8; 16];
        let len = z2m_rs::encode(self.profile, &mapped, &mut bytes).ok_or(Error::InvalidSize)?;
        let payload = Payload::new(&bytes[..len]).map_err(|_| Error::InvalidSize)?;
        let request = Request {
            device: self.device.0,
            short: self.device.1,
            endpoint: self.endpoint,
            sleepy: false,
            retry: RetryPolicy::SAFE,
            expiry_ms: 10_000,
            timeout_ms: 3_000,
            kind: RequestKind::SendZcl {
                cluster,
                command: zcl_command,
                payload,
                manufacturer: None,
                frame_control: 0x01,
                expect: Expect::DefaultResponse {
                    to_command: zcl_command,
                },
            },
        };
        match self.executor.enqueue(request, now) {
            Ok(id) => Ok(OperationToken::from(id)),
            Err(ExecError::QueueFull) | Err(ExecError::ResultQueueFull) => Err(Error::NoSpace),
            Err(ExecError::PayloadTooLarge) | Err(ExecError::TooManyAttributes) => {
                Err(Error::InvalidSize)
            }
        }
    }
}

#[derive(Clone, Copy)]
pub struct Binding {
    pub entity: EntityId,
    pub eui: u64,
    pub short: u16,
    pub endpoint: u8,
    pub profile: &'static Profile,
    pub standard: Option<EndpointDescriptor>,
}

/// `short` is a network address and changes on rejoin; the EUI64 and entity are
/// stable, so refresh every matching binding instead of re-registering it.
pub fn update_binding_short(bindings: &mut [Binding], eui: u64, short: u16) -> bool {
    let mut changed = false;
    for binding in bindings.iter_mut().filter(|binding| binding.eui == eui) {
        if binding.short != short {
            binding.short = short;
            changed = true;
        }
    }
    changed
}

/// Single command entry: routes a canonical request by entity id to the bound
/// device and profile, checks capabilities, then uses CommandCore/ZigbeeSink.
pub struct CommandRouter<'a> {
    pub executor: &'a mut Executor,
    pub bindings: &'a [Binding],
}

impl<'a> CommandRouter<'a> {
    pub fn new(executor: &'a mut Executor, bindings: &'a [Binding]) -> Self {
        Self { executor, bindings }
    }

    pub fn submit(
        &mut self,
        core: &mut CommandCore,
        entity: EntityId,
        property: PropertyId,
        value: CommandValue,
        now: u64,
    ) -> Result<OperationToken, Error> {
        let binding = self
            .bindings
            .iter()
            .find(|binding| binding.entity == entity)
            .copied()
            .ok_or(Error::NotFound)?;
        let action = command_action(property, &value).ok_or(Error::InvalidArg)?;
        if !capability_allows(binding.profile, binding.standard.as_ref(), property, action) {
            return Err(Error::InvalidArg);
        }
        let mut sink = ZigbeeSink {
            executor: self.executor,
            profile: binding.profile,
            device: (binding.eui, binding.short),
            endpoint: binding.endpoint,
        };
        core.submit(
            &mut sink,
            CanonicalCommand {
                entity,
                property,
                action,
                value,
            },
            now,
        )
    }
}

fn command_action(property: PropertyId, value: &CommandValue) -> Option<u32> {
    if property == PROPERTY_POWER {
        return match value {
            CommandValue::Scalar(v) => v.as_bool().map(|on| if on { ACTION_ON } else { ACTION_OFF }),
            CommandValue::None | CommandValue::Xy { .. } => None,
        };
    }
    Some(ACTION_SET)
}

fn capability_allows(
    profile: &Profile,
    standard_endpoint: Option<&EndpointDescriptor>,
    property: PropertyId,
    action: u32,
) -> bool {
    if let Some(endpoint) = standard_endpoint {
        return standard::capability_allows(endpoint, property, action);
    }
    let caps = profile_capabilities(profile);
    caps.properties[..usize::from(caps.property_count)]
        .iter()
        .any(|capability| {
            capability.property == property
                && capability.actions[..usize::from(capability.action_count)].contains(&action)
        })
}
pub fn status_from_exec(status: Status) -> Option<CommandStatus> {
    Some(match status {
        Status::Queued | Status::Sent => CommandStatus::Sent,
        Status::Acknowledged | Status::Confirmed => CommandStatus::Acknowledged,
        Status::Timeout => CommandStatus::Timeout,
        Status::Failed | Status::Expired => CommandStatus::Failed,
    })
}
