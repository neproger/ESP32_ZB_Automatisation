#![forbid(unsafe_code)]

//! Bounded command intake for the Zigbee/HA layer.
//!
//! HTTP handlers only enqueue; the commands task owns Domain, CommandCore and
//! the executor, so a command is never sent while a device is being discovered
//! or while its capabilities are still unconfirmed.

use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::Mutex;

use ha_domain::canonical::{ActionId, CommandValue, EntityId, PropertyId};
use ha_domain::Error;

const CAPACITY: usize = 8;

static SENDER: Mutex<Option<SyncSender<Request>>> = Mutex::new(None);

pub enum Request {
    Command {
        entity: EntityId,
        property: PropertyId,
        action: ActionId,
        value: CommandValue,
    },
    RenameEntity {
        entity: EntityId,
        name: String,
    },
    RenameDevice {
        device: u64,
        name: String,
    },
    RemoveDevice {
        device: u64,
    },
    /// Generic canonical row: groups, group items/automation links, rules.
    Upsert {
        entity: u32,
        key: Vec<u8>,
        record: Vec<u8>,
    },
    Remove {
        entity: u32,
        key: Vec<u8>,
    },
}

pub fn start() -> Receiver<Request> {
    let (sender, receiver) = sync_channel(CAPACITY);
    *SENDER.lock().unwrap() = Some(sender);
    receiver
}

fn queue(request: Request) -> Result<(), Error> {
    let guard = SENDER.lock().unwrap();
    let Some(sender) = guard.as_ref() else {
        return Err(Error::InvalidState);
    };
    sender.try_send(request).map_err(|_| Error::NoSpace)
}

pub fn command(
    entity: EntityId,
    property: PropertyId,
    action: ActionId,
    value: CommandValue,
) -> Result<(), Error> {
    queue(Request::Command {
        entity,
        property,
        action,
        value,
    })
}

pub fn rename_entity(entity: EntityId, name: String) -> Result<(), Error> {
    queue(Request::RenameEntity { entity, name })
}

pub fn rename_device(device: u64, name: String) -> Result<(), Error> {
    queue(Request::RenameDevice { device, name })
}

pub fn remove_device(device: u64) -> Result<(), Error> {
    queue(Request::RemoveDevice { device })
}

pub fn upsert(entity: u32, key: Vec<u8>, record: Vec<u8>) -> Result<(), Error> {
    queue(Request::Upsert {
        entity,
        key,
        record,
    })
}

pub fn remove(entity: u32, key: Vec<u8>) -> Result<(), Error> {
    queue(Request::Remove { entity, key })
}
