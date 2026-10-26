//! Bounded command intake for the Zigbee/HA layer.
//!
//! HTTP handlers only enqueue; the main loop owns Domain, CommandCore and the
//! executor, so a command is never sent while a device is being discovered or
//! while its capabilities are still unconfirmed.

use std::collections::VecDeque;
use std::sync::Mutex;

use ha_domain::canonical::{ActionId, CommandValue, EntityId, PropertyId};
use ha_domain::Error;

const CAPACITY: usize = 8;

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
    SetAutomationEnabled {
        automation: u64,
        enabled: bool,
    },
    SetSettings {
        brightness: u8,
    },
    WifiConnect {
        ssid: String,
        password: String,
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

static QUEUE: Mutex<VecDeque<Request>> = Mutex::new(VecDeque::new());

fn queue(request: Request) -> Result<(), Error> {
    let mut queue = QUEUE.lock().unwrap();
    if queue.len() == CAPACITY {
        return Err(Error::NoSpace);
    }
    queue.push_back(request);
    Ok(())
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

pub fn set_automation_enabled(automation: u64, enabled: bool) -> Result<(), Error> {
    queue(Request::SetAutomationEnabled {
        automation,
        enabled,
    })
}

pub fn set_settings(brightness: u8) -> Result<(), Error> {
    queue(Request::SetSettings { brightness })
}

pub fn wifi_connect(ssid: String, password: String) -> Result<(), Error> {
    queue(Request::WifiConnect { ssid, password })
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

pub fn take() -> Option<Request> {
    QUEUE.lock().unwrap().pop_front()
}
