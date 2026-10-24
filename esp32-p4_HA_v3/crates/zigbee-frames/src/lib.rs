//! Fixed frame pool for the C -> Rust boundary and ZCL attribute frame access.
//!
//! One bounded copy happens in the C callback into a pool slot; from then on the
//! frame is parsed in place and `z2m-rs` receives borrowed attribute views. The
//! pool never grows: a slot is reused only after its RAII guard is dropped.
//!
//! Error classes are kept apart:
//! * [`PoolError::Exhausted`] — no free slot (queue pressure),
//! * [`PoolError::TooLarge`] — frame exceeds the slot capacity,
//! * [`z2m_rs::FrameError`] — the frame itself is malformed/unsupported.

use core::cell::Cell;
use std::cell::{Ref, RefCell};

use z2m_rs::{AttributeFrameView, FrameError};

pub const POOL_SLOTS: usize = 8;
pub const FRAME_CAPACITY: usize = 160;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PoolError {
    Exhausted,
    TooLarge,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ZclFrame<const CAP: usize> {
    pub cluster: u16,
    pub endpoint: u8,
    pub command: u8,
    len: usize,
    bytes: [u8; CAP],
}

impl<const CAP: usize> Default for ZclFrame<CAP> {
    fn default() -> Self {
        Self {
            cluster: 0,
            endpoint: 0,
            command: 0,
            len: 0,
            bytes: [0; CAP],
        }
    }
}

impl<const CAP: usize> ZclFrame<CAP> {
    /// Copies one frame into the slot; the only copy on this path.
    pub fn set(
        &mut self,
        cluster: u16,
        endpoint: u8,
        command: u8,
        data: &[u8],
    ) -> Result<(), PoolError> {
        if data.len() > CAP {
            return Err(PoolError::TooLarge);
        }
        self.cluster = cluster;
        self.endpoint = endpoint;
        self.command = command;
        self.len = data.len();
        self.bytes[..data.len()].copy_from_slice(data);
        Ok(())
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }

    pub fn parse(&self) -> Result<AttributeFrameView<'_>, FrameError> {
        AttributeFrameView::parse(self.cluster, self.endpoint, self.command, self.bytes())
    }
}

pub struct FramePool<const SLOTS: usize, const CAP: usize> {
    slots: [RefCell<ZclFrame<CAP>>; SLOTS],
    used: [Cell<bool>; SLOTS],
}

impl<const SLOTS: usize, const CAP: usize> Default for FramePool<SLOTS, CAP> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const SLOTS: usize, const CAP: usize> FramePool<SLOTS, CAP> {
    pub fn new() -> Self {
        Self {
            slots: std::array::from_fn(|_| RefCell::new(ZclFrame::default())),
            used: std::array::from_fn(|_| Cell::new(false)),
        }
    }

    /// Takes a free slot. The slot returns to the pool when the guard is dropped.
    pub fn acquire(&self) -> Result<FrameGuard<'_, CAP>, PoolError> {
        for index in 0..SLOTS {
            if !self.used[index].get() {
                self.used[index].set(true);
                *self.slots[index].borrow_mut() = ZclFrame::default();
                return Ok(FrameGuard {
                    slot: &self.slots[index],
                    used: &self.used[index],
                });
            }
        }
        Err(PoolError::Exhausted)
    }

    pub fn free_slots(&self) -> usize {
        self.used.iter().filter(|used| !used.get()).count()
    }
}

pub struct FrameGuard<'a, const CAP: usize> {
    slot: &'a RefCell<ZclFrame<CAP>>,
    used: &'a Cell<bool>,
}

impl<const CAP: usize> FrameGuard<'_, CAP> {
    pub fn write(
        &self,
        cluster: u16,
        endpoint: u8,
        command: u8,
        data: &[u8],
    ) -> Result<(), PoolError> {
        self.slot.borrow_mut().set(cluster, endpoint, command, data)
    }

    pub fn frame(&self) -> Ref<'_, ZclFrame<CAP>> {
        self.slot.borrow()
    }

    /// Parses in place and runs `visit` with borrowed attribute views.
    pub fn with_view<R>(
        &self,
        visit: impl FnOnce(&AttributeFrameView<'_>) -> R,
    ) -> Result<R, FrameError> {
        let frame = self.slot.borrow();
        let view = frame.parse()?;
        Ok(visit(&view))
    }
}

impl<const CAP: usize> Drop for FrameGuard<'_, CAP> {
    fn drop(&mut self) {
        self.used.set(false);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use z2m_rs::{decode_frame, properties, Decoded, Update, Value, SNZB_01P};

    const BATTERY_REPORT: [u8; 8] = [0x21, 0x00, 0x20, 200, 0x20, 0x00, 0x20, 30];

    #[test]
    fn slot_returns_to_pool_after_drop_and_starts_empty() {
        let pool: FramePool<2, FRAME_CAPACITY> = FramePool::new();
        assert_eq!(pool.free_slots(), 2);
        {
            let slot = pool.acquire().unwrap();
            slot.write(0x0001, 1, 0x0A, &BATTERY_REPORT).unwrap();
            assert_eq!(pool.free_slots(), 1);
        }
        assert_eq!(pool.free_slots(), 2);
        let slot = pool.acquire().unwrap();
        assert!(slot.frame().bytes().is_empty());
    }

    #[test]
    fn exhaustion_is_explicit_and_independent_of_frame_size() {
        let pool: FramePool<2, FRAME_CAPACITY> = FramePool::new();
        let _a = pool.acquire().unwrap();
        let _b = pool.acquire().unwrap();
        assert_eq!(pool.acquire().err(), Some(PoolError::Exhausted));

        let small: FramePool<1, 4> = FramePool::new();
        let slot = small.acquire().unwrap();
        assert_eq!(slot.write(0x0006, 1, 0x01, &[0; 5]), Err(PoolError::TooLarge));
    }

    #[test]
    fn raii_returns_slot_on_early_exit() {
        let pool: FramePool<1, FRAME_CAPACITY> = FramePool::new();
        let result = (|| {
            let _slot = pool.acquire()?;
            Err::<(), PoolError>(PoolError::Exhausted)
        })();
        assert_eq!(result, Err(PoolError::Exhausted));
        assert_eq!(pool.free_slots(), 1);
    }

    #[test]
    fn multi_attribute_frame_reaches_z2m_rs() {
        let pool: FramePool<1, FRAME_CAPACITY> = FramePool::new();
        let slot = pool.acquire().unwrap();
        slot.write(0x0001, 1, 0x0A, &BATTERY_REPORT).unwrap();
        let decoded = slot
            .with_view(|view| {
                let mut out = Decoded::default();
                assert_eq!(decode_frame(&SNZB_01P, view, &mut out), 2);
                out.updates().copied().collect::<Vec<_>>()
            })
            .unwrap();
        assert_eq!(
            decoded,
            vec![
                Update::Property {
                    property: properties::BATTERY,
                    value: Value::Float(100.0)
                },
                Update::Property {
                    property: properties::VOLTAGE,
                    value: Value::Float(3000.0)
                }
            ]
        );
    }

    #[test]
    fn malformed_frame_is_not_a_pool_error() {
        let pool: FramePool<1, FRAME_CAPACITY> = FramePool::new();
        let slot = pool.acquire().unwrap();
        slot.write(0x0001, 1, 0x0A, &[0x21, 0x00, 0x21, 1]).unwrap();
        assert_eq!(slot.with_view(|_| ()), Err(FrameError::Truncated));
        assert_eq!(pool.free_slots(), 0, "guard still owns the slot");
    }

    #[test]
    fn unknown_attribute_ids_produce_no_updates() {
        let pool: FramePool<1, FRAME_CAPACITY> = FramePool::new();
        let slot = pool.acquire().unwrap();
        slot.write(0x0001, 1, 0x0A, &[0x55, 0x00, 0x20, 1]).unwrap();
        let added = slot
            .with_view(|view| {
                let mut out = Decoded::default();
                decode_frame(&SNZB_01P, view, &mut out)
            })
            .unwrap();
        assert_eq!(added, 0);
    }
}
