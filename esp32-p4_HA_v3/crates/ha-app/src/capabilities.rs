//! Capability hand-off from the configure runner to the Domain adapter.
//!
//! State and events do not pass through this module: they are decoded and
//! written to Domain directly by `frames::IngestSink`. Only capability
//! reconfiguration uses a small bounded mailbox until configure gets a direct
//! Domain capability sink.

use std::sync::Mutex;

use ha_domain::canonical::CapabilitiesRecord;

const MAX_ITEMS: usize = 32;

static CAPABILITIES: Mutex<Vec<(u64, CapabilitiesRecord)>> = Mutex::new(Vec::new());

pub fn push_capabilities(entity: u64, capabilities: CapabilitiesRecord) {
    let mut queue = CAPABILITIES.lock().unwrap();
    if queue.len() < MAX_ITEMS {
        if let Some(slot) = queue.iter_mut().find(|(id, _)| *id == entity) {
            slot.1 = capabilities;
        } else {
            queue.push((entity, capabilities));
        }
    }
}

pub fn take_capabilities() -> Option<(u64, CapabilitiesRecord)> {
    let mut queue = CAPABILITIES.lock().unwrap();
    if queue.is_empty() {
        None
    } else {
        Some(queue.remove(0))
    }
}
