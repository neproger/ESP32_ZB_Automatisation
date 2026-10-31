use super::*;

/// Optional P4-only measurements around every Domain lock acquisition.
#[cfg(feature = "p4-lock-metrics")]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LockMetrics {
    pub locks: u32,
    pub wait_total_us: u32,
    pub wait_max_us: u32,
    pub hold_total_us: u32,
    pub hold_max_us: u32,
}

#[cfg(feature = "p4-lock-metrics")]
static LOCK_COUNT: AtomicU32 = AtomicU32::new(0);
#[cfg(feature = "p4-lock-metrics")]
static LOCK_WAIT_TOTAL_US: AtomicU32 = AtomicU32::new(0);
#[cfg(feature = "p4-lock-metrics")]
static LOCK_WAIT_MAX_US: AtomicU32 = AtomicU32::new(0);
#[cfg(feature = "p4-lock-metrics")]
static LOCK_HOLD_TOTAL_US: AtomicU32 = AtomicU32::new(0);
#[cfg(feature = "p4-lock-metrics")]
static LOCK_HOLD_MAX_US: AtomicU32 = AtomicU32::new(0);

#[cfg(feature = "p4-lock-metrics")]
pub(crate) fn elapsed_us(start: Instant) -> u32 {
    start.elapsed().as_micros().min(u128::from(u32::MAX)) as u32
}

#[cfg(feature = "p4-lock-metrics")]
fn add_saturating(value: &AtomicU32, amount: u32) {
    let mut current = value.load(AtomicOrdering::Relaxed);
    loop {
        let next = current.saturating_add(amount);
        match value.compare_exchange_weak(
            current,
            next,
            AtomicOrdering::Relaxed,
            AtomicOrdering::Relaxed,
        ) {
            Ok(_) => break,
            Err(observed) => current = observed,
        }
    }
}

#[cfg(feature = "p4-lock-metrics")]
pub(crate) fn record_wait(value: u32) {
    add_saturating(&LOCK_COUNT, 1);
    add_saturating(&LOCK_WAIT_TOTAL_US, value);
    LOCK_WAIT_MAX_US.fetch_max(value, AtomicOrdering::Relaxed);
}

#[cfg(feature = "p4-lock-metrics")]
fn record_hold(value: u32) {
    add_saturating(&LOCK_HOLD_TOTAL_US, value);
    LOCK_HOLD_MAX_US.fetch_max(value, AtomicOrdering::Relaxed);
}

#[cfg(feature = "p4-lock-metrics")]
pub fn p4_lock_metrics() -> LockMetrics {
    LockMetrics {
        locks: LOCK_COUNT.load(AtomicOrdering::Relaxed),
        wait_total_us: LOCK_WAIT_TOTAL_US.load(AtomicOrdering::Relaxed),
        wait_max_us: LOCK_WAIT_MAX_US.load(AtomicOrdering::Relaxed),
        hold_total_us: LOCK_HOLD_TOTAL_US.load(AtomicOrdering::Relaxed),
        hold_max_us: LOCK_HOLD_MAX_US.load(AtomicOrdering::Relaxed),
    }
}

#[cfg(feature = "p4-lock-metrics")]
pub fn p4_lock_metrics_reset() {
    LOCK_COUNT.store(0, AtomicOrdering::Relaxed);
    LOCK_WAIT_TOTAL_US.store(0, AtomicOrdering::Relaxed);
    LOCK_WAIT_MAX_US.store(0, AtomicOrdering::Relaxed);
    LOCK_HOLD_TOTAL_US.store(0, AtomicOrdering::Relaxed);
    LOCK_HOLD_MAX_US.store(0, AtomicOrdering::Relaxed);
}

#[cfg(feature = "p4-lock-metrics")]
pub(crate) struct MeasuredGuard<'a> {
    pub(crate) guard: MutexGuard<'a, Domain>,
    pub(crate) holding_since: Instant,
}

#[cfg(feature = "p4-lock-metrics")]
impl std::ops::Deref for MeasuredGuard<'_> {
    type Target = Domain;

    fn deref(&self) -> &Domain {
        &self.guard
    }
}

#[cfg(feature = "p4-lock-metrics")]
impl std::ops::DerefMut for MeasuredGuard<'_> {
    fn deref_mut(&mut self) -> &mut Domain {
        &mut self.guard
    }
}

#[cfg(feature = "p4-lock-metrics")]
impl Drop for MeasuredGuard<'_> {
    fn drop(&mut self) {
        record_hold(elapsed_us(self.holding_since));
    }
}

#[cfg(feature = "p4-lock-metrics")]
pub(crate) type DomainGuard<'a> = MeasuredGuard<'a>;

#[cfg(not(feature = "p4-lock-metrics"))]
pub(crate) type DomainGuard<'a> = MutexGuard<'a, Domain>;
