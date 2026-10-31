//! Fixed-capacity storage primitives, host-verifiable.
//!
//! Contracts follow V2 `mstore` (`docs/storage/MSTORE.md`, C `mstore` component):
//!
//! * canonical state is a fixed array of slots: `meta + key + payload`;
//! * `meta.generation` identifies one life of a physical slot, `meta.version` one
//!   payload revision inside it;
//! * `key` is fixed for a slot life; identity change is free + allocate;
//! * index / free list / live count are derived and rebuildable at any time;
//! * mutations take a [`Handle`] (`slot + generation + version`) and reject stale
//!   handles; `Table::check` classifies removed/stale/unchanged/changed;
//! * reads go through `Storage` buffers; iteration is callback-based;
//! * capacity, index and free list are allocated once.
//!
//! Backends (`RAM`, `Flash`, `RAM + Flash`) keep canonical slot bytes and do not
//! know what keys or generations mean.

mod flash;
mod ring;
mod storage;
mod table;
pub use flash::sim::SimFlash;
pub use flash::{FlashError, FlashMeta, FlashTable, Geometry, NorFlash};
pub use storage::{FlashStorage, RamFlashStorage, RamStorage, Storage};

pub use ring::Ring;
pub use table::{Error, Handle, HandleStatus, Meta, Result, Table};
