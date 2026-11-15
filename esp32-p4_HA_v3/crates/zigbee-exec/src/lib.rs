#![forbid(unsafe_code)]

//! Request Executor: reliable delivery of Zigbee operations.
//!
//! Ported model of `zigbee-herdsman` request handling: per device/endpoint
//! serialization, AF-confirm with the SDK-assigned TSN, response waiters keyed by
//! the full context `(short address, endpoint, cluster, command, tsn)`, bounded
//! early-response buffer (response before confirm), expiry, selective retry and
//! sleepy parking.
//!
//! It does not know profiles or devices: `z2m-rs` builds requests, the HA layer
//! schedules them, a transport (fake here, Espressif SDK later) sends them.
//!
//! Completion is defined per request, not as one fixed state chain:
//! * `Expect::None` finishes as `Acknowledged` on a successful AF confirm;
//! * `ZclResponse`/`DefaultResponse` finishes as `Confirmed` (or `Failed`);
//! * ZDO bind finishes on the SDK callback, without a TSN.
//!
//! Acknowledged delivery is never treated as confirmed device state.

mod types;
mod executor;
pub mod fake;
#[cfg(test)]
mod tests;

pub use executor::Executor;
pub use types::*;
