//! Fixed-capacity storage primitives, host-verifiable.
//!
//! Contracts follow V2 `mstore` (`docs/storage/MSTORE.md`, C `mstore` component):
//!
//! * canonical state is a fixed array of slots: `meta + key + payload`;
//! * `meta.generation` identifies one life of a physical slot, `meta.version` one
//!   payload revision inside it;
//! * `key` is fixed for a slot life; identity change is free + allocate;
//! * index / free list / live count are derived and rebuildable at any time;
//! * mutations take a [`Handle`] (`slot + generation`) and reject stale handles;
//! * reads borrow the slot directly, so payload is never copied;
//! * no hidden allocation: capacity, index and free list are allocated once.
//!
//! Backends (RAM now, NOR flash later) keep canonical slot bytes and do not know
//! what keys or generations mean.

mod flash;
mod persisted;
mod ring;
mod table;
pub use flash::{FlashError, FlashMeta, FlashTable, Geometry, NorFlash};
pub use flash::sim::SimFlash;
pub use persisted::{Fixed, PersistedTable};

pub use ring::Ring;
pub use table::{Error, Handle, Meta, Result, Table, View};
