//! HA Zigbee Layer: canonical device record and the persistent registry.
//!
//! Device semantics (fingerprints, converters, configure plans) live in
//! `z2m-rs`; this crate holds the HA-side record layout and the registry over
//! MicroDB. No model-specific decoding belongs here.

mod device_record;
mod registry;

pub use device_record::{
    DeviceRecord, InterviewStatus, RecordError, DEVICE_SCHEMA, HEADER_LEN, MAX_RECORD,
};
pub use registry::{RecordSlot, Registry};
