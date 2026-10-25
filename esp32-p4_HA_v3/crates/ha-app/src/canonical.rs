//! Bounded hand-off from the Zigbee decode path to the Domain adapter.
//! States and events are separate: an event is never stored as property state.

use std::sync::Mutex;

use ha_domain::canonical::EventId;
use zigbee_devices::StateUpdate;

const MAX_ITEMS: usize = 32;

#[derive(Clone, Copy)]
pub struct CanonicalEvent {
    pub entity: u64,
    pub event: EventId,
}

static STATES: Mutex<Vec<StateUpdate>> = Mutex::new(Vec::new());
static EVENTS: Mutex<Vec<CanonicalEvent>> = Mutex::new(Vec::new());

pub fn push(entity: u64, update: &z2m_rs::Update) {
    let (states, event) = zigbee_devices::canonical_states(entity, update);
    {
        let mut queue = STATES.lock().unwrap();
        for state in states.into_iter().flatten() {
            if queue.len() < MAX_ITEMS {
                queue.push(state);
            }
        }
    }
    if let Some(event) = event {
        let mut queue = EVENTS.lock().unwrap();
        if queue.len() < MAX_ITEMS {
            queue.push(CanonicalEvent { entity, event });
        }
    }
}

pub fn take_state() -> Option<StateUpdate> {
    let mut queue = STATES.lock().unwrap();
    if queue.is_empty() {
        None
    } else {
        Some(queue.remove(0))
    }
}

pub fn take_event() -> Option<CanonicalEvent> {
    let mut queue = EVENTS.lock().unwrap();
    if queue.is_empty() {
        None
    } else {
        Some(queue.remove(0))
    }
}
