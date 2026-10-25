//! Bridge between HA Command Core and the universal request executor.

use ha_domain::canonical::CanonicalCommand;
use ha_domain::command::{CommandSink, CommandStatus, OperationToken};
use ha_domain::Error;
use zigbee_exec::{
    ExecError, Executor, Expect, Payload, Request, RequestKind, RetryPolicy, Status,
};

use crate::canonical::canonical_command;
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

pub fn status_from_exec(status: Status) -> Option<CommandStatus> {
    Some(match status {
        Status::Queued | Status::Sent => CommandStatus::Sent,
        Status::Acknowledged | Status::Confirmed => CommandStatus::Acknowledged,
        Status::Timeout => CommandStatus::Timeout,
        Status::Failed | Status::Expired => CommandStatus::Failed,
    })
}
