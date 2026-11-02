//! Host-run coverage for the ha-app bounded removal queue.
//!
//! `ha-app` is outside the workspace, so include the dependency-free queue
//! module directly instead of duplicating its implementation.

#[path = "../../ha-app/src/removal_queue.rs"]
mod removal_queue;

use removal_queue::{RemovalQueue, REMOVAL_QUEUE_CAPACITY};

#[test]
fn removal_queue_is_bounded_and_does_not_drop_requests() {
    let mut queue = RemovalQueue::new();
    for eui in 1..=REMOVAL_QUEUE_CAPACITY as u64 {
        assert!(queue.try_push(eui).is_ok());
    }
    assert_eq!(
        queue.try_push(REMOVAL_QUEUE_CAPACITY as u64 + 1),
        Err(ha_domain::Error::NoSpace)
    );

    let (items, count) = queue.take_all();
    assert_eq!(count, REMOVAL_QUEUE_CAPACITY);
    assert_eq!(&items[..count], &[1, 2, 3, 4, 5, 6, 7, 8]);
    assert!(queue.try_push(9).is_ok());
}
