//! Bounded command intake for the Zigbee layer.
//!
//! HTTP/automation code only enqueues; the main loop owns CommandCore and the
//! executor, so a command is never sent while a device is being discovered or
//! while its capabilities are still unconfirmed.

use std::collections::VecDeque;
use std::sync::Mutex;

use ha_domain::canonical::{ActionId, CommandValue, EntityId, PropertyId};
use ha_domain::Error;

const CAPACITY: usize = 8;

#[derive(Clone, Copy)]
pub struct Request {
    pub entity: EntityId,
    pub property: PropertyId,
    pub action: ActionId,
    pub value: CommandValue,
}

static QUEUE: Mutex<VecDeque<Request>> = Mutex::new(VecDeque::new());

pub fn enqueue(
    entity: EntityId,
    property: PropertyId,
    action: ActionId,
    value: CommandValue,
) -> Result<(), Error> {
    let mut queue = QUEUE.lock().unwrap();
    if queue.len() == CAPACITY {
        return Err(Error::NoSpace);
    }
    queue.push_back(Request {
        entity,
        property,
        action,
        value,
    });
    Ok(())
}

pub fn take() -> Option<Request> {
    QUEUE.lock().unwrap().pop_front()
}
