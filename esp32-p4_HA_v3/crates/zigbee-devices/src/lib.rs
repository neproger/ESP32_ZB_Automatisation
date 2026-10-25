//! HA Zigbee Layer: canonical device record and the persistent registry.
//!
//! Device semantics (fingerprints, converters, configure plans) live in
//! `z2m-rs`; this crate holds the HA-side record layout and the registry over
//! MicroDB. No model-specific decoding belongs here.

mod command;
mod config_run;
mod descriptor;
mod device_record;
mod domain_adapter;
mod interview;
mod ownership;
mod projection;
mod registry;
pub mod standard;

pub use command::{status_from_exec, update_binding_short, Binding, CommandRouter, ZigbeeSink};
pub use config_run::{ConfigRun, RunOptions};
pub use descriptor::{
    ClusterEntry, ClusterRole, DescriptorError, DeviceDescriptor, EndpointDescriptor, MAX_CLUSTERS,
    MAX_ENDPOINTS, SIGNATURE_MAX,
};
pub use device_record::{
    DeviceRecord, InterviewStatus, RecordError, DEVICE_SCHEMA, HEADER_LEN, MAX_RECORD,
};
pub use domain_adapter::{DomainAdapter, RecordSink};
pub use interview::{
    Identified, InterviewManager, COOLDOWN_MS, JOB_SLOTS, MAX_ATTEMPTS, OP_DEADLINE_MS,
    RETRY_BACKOFF_MS,
};
pub use ownership::{Claim, ClaimKey, ClaimKind, Claims, Owner, Resolution, MAX_CLAIMS};
pub use projection::{
    canonical_command, canonical_states, encode_canonical_command, profile_capabilities,
    zigbee_device_id, zigbee_entity_id, StateUpdate,
};
pub use registry::{RecordSlot, Registry};
#[allow(unused_imports)]
pub use standard::STANDARD_PROFILE;
pub use standard::{
    attribute_capability, capabilities_confirmed, capability_allows, classify_endpoint,
    decode_standard, update_capability, update_is_discovered, Confirmed, DeviceKind, EndpointClass,
    EndpointFeatures, StandardCapability, MAX_CONFIRMED_ATTRIBUTES, STANDARD_PROFILE_ID,
};
