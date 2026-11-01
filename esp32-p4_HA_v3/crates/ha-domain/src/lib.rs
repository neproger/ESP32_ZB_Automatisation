//! Domain core: entity storage, journal of facts, fan-out to subscribers.
//!
//! Semantics follow V2 `domain`: the core stores facts and delivers them, it does
//! not decide what they mean. Entity tables are addressed by an opaque type id and
//! fixed-size key/record bytes; keys and records stay in one binary form shared by
//! storage, UART and WebSocket.
//!
//! * put/remove return `changed` and write the fact only on a real change;
//! * `version` grows only when the record actually changed;
//! * the journal is a bounded ring; the dispatcher starts at the current head and
//!   counts gaps instead of replaying history;
//! * subscribers only copy a fact into their inbox (V2 `try_push` contract).

pub use ha_model::canonical;
mod command;
mod dispatch;
mod domain;
mod entity;
mod fact;
mod handle;
mod journal;
mod metrics;
mod storage;
mod transaction;

use command::Command;
use dispatch::{DispatchSignal, Subscription};

pub use dispatch::DispatchHandle;

pub use domain::Domain;
pub use entity::{EntitySnapshot, ENTITY_SNAPSHOT_MAX_BYTES};
pub use handle::DomainHandle;
pub use transaction::DomainTransaction;

#[cfg(not(feature = "p4-lock-metrics"))]
use metrics::DomainGuard;
#[cfg(feature = "p4-lock-metrics")]
use metrics::{elapsed_us, record_wait, DomainGuard, MeasuredGuard};
#[cfg(feature = "p4-lock-metrics")]
pub use metrics::{p4_lock_metrics, p4_lock_metrics_reset, LockMetrics};

#[cfg(feature = "p4-lock-metrics")]
use std::sync::atomic::{AtomicU32, Ordering as AtomicOrdering};
use std::sync::{Arc, Condvar, Mutex as StdMutex, MutexGuard};
#[cfg(feature = "p4-lock-metrics")]
use std::time::Instant;
use std::time::{SystemTime, UNIX_EPOCH};

use micro_db::{Geometry, Handle, RamStorage, Ring, Table};

pub use entity::{EntityDesc, EntityMeta};
pub use storage::{
    Backing, DomainMaintenance, EntityRestore, RestoreError, RestorePhase, RestoreReport,
    SharedStorage, StorageBackend, StorageBackendImpl, StorageProvider,
};

pub use fact::{
    EntityType, EventId, Fact, FactKind, FactMeta, FactTarget, Filter, Op, PayloadRef, Value,
    ValueType, FACT_KEY_MAX,
};
pub use micro_db::Error;

pub type Result<T> = core::result::Result<T, Error>;

#[cfg(test)]
mod tests;
