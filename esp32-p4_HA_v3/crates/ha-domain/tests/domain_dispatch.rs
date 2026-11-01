use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{sync_channel, Receiver};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use ha_domain::{Backing, Domain, DomainHandle, EntityDesc, Fact, FactMeta, Filter, Value};

const ENTITY: u32 = 7;

fn meta() -> FactMeta {
    FactMeta {
        source: 1,
        value: Value::NONE,
    }
}

fn harness() -> (DomainHandle, Receiver<Fact>) {
    let mut domain = Domain::new(256, 16, 64).unwrap();
    domain
        .register_entity(EntityDesc {
            entity: ENTITY,
            key_size: 8,
            record_size: 8,
            capacity: 256,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    let (sender, receiver) = sync_channel(64);
    domain.subscribe_channel(Filter::all(), sender);
    (DomainHandle::new(domain), receiver)
}

fn harness_two() -> (DomainHandle, Receiver<Fact>, Receiver<Fact>) {
    let mut domain = Domain::new(256, 16, 64).unwrap();
    domain
        .register_entity(EntityDesc {
            entity: ENTITY,
            key_size: 8,
            record_size: 8,
            capacity: 256,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    let (first_sender, first_receiver) = sync_channel(64);
    let (second_sender, second_receiver) = sync_channel(64);
    domain.subscribe_channel(Filter::all(), first_sender);
    domain.subscribe_channel(Filter::all(), second_sender);
    (DomainHandle::new(domain), first_receiver, second_receiver)
}

fn put(domain: &DomainHandle, key: u64, value: u8) -> bool {
    domain
        .entity_put(ENTITY, &key.to_le_bytes(), &[value; 8], meta())
        .expect("entity_put")
}

fn wait_until(mut condition: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while !condition() {
        assert!(Instant::now() < deadline, "condition timed out");
        thread::sleep(Duration::from_millis(5));
    }
}

fn assert_completes(label: &'static str, operation: impl FnOnce() + Send + 'static) {
    let (done_tx, done_rx) = sync_channel(0);
    let worker = thread::spawn(move || {
        operation();
        done_tx.send(()).unwrap();
    });
    done_rx
        .recv_timeout(Duration::from_secs(5))
        .unwrap_or_else(|_| panic!("{label} did not complete"));
    // On timeout the worker is intentionally detached; it may be blocked by
    // the failure under test, so joining it would hide the reported failure.
    worker.join().unwrap();
}

struct Dispatcher {
    stop: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

impl Dispatcher {
    fn start(domain: &DomainHandle) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let running = Arc::clone(&stop);
        let worker = domain.clone();
        let join = thread::spawn(move || {
            while !running.load(Ordering::Relaxed) {
                let _ = worker.dispatch_once();
                thread::sleep(Duration::from_millis(1));
            }
        });
        Self {
            stop,
            join: Some(join),
        }
    }

    fn shutdown(mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(join) = self.join.take() {
            join.join().unwrap();
        }
    }
}

impl Drop for Dispatcher {
    fn drop(&mut self) {
        // Do not join here: a failed test may expose a blocked dispatcher.
        self.stop.store(true, Ordering::Relaxed);
    }
}

#[test]
fn empty_inbox_is_empty() {
    let mut domain = Domain::new(8, 4, 8).unwrap();
    let (sender, receiver) = sync_channel(4);
    domain.subscribe_channel(Filter::all(), sender);
    assert_eq!(domain.dispatch_once(), 0);
    assert_eq!(
        receiver.try_recv(),
        Err(std::sync::mpsc::TryRecvError::Empty)
    );
}

#[test]
fn multiple_subscribers_receive_independent_ordered_facts() {
    let (domain, receiver, second_receiver) = harness_two();

    assert!(put(&domain, 1, 0x11));
    assert!(put(&domain, 2, 0x22));
    assert_eq!(domain.dispatch_once().unwrap(), 4);

    let first = receiver.try_recv().unwrap();
    let second = receiver.try_recv().unwrap();
    let first_other = second_receiver.try_recv().unwrap();
    let second_other = second_receiver.try_recv().unwrap();

    assert_eq!(
        (first.event_id, second.event_id),
        (1, 2),
        "first inbox order"
    );
    assert_eq!(
        (first_other.event_id, second_other.event_id),
        (1, 2),
        "second inbox order"
    );
    assert_eq!(first.key(), first_other.key());
    assert_eq!(second.key(), second_other.key());
    assert_eq!(
        receiver.try_recv(),
        Err(std::sync::mpsc::TryRecvError::Empty)
    );
    assert_eq!(
        second_receiver.try_recv(),
        Err(std::sync::mpsc::TryRecvError::Empty)
    );
}

#[test]
fn fast_consumer_receives_all_facts() {
    let (domain, receiver) = harness();
    let dispatcher = Dispatcher::start(&domain);
    let seen = Arc::new(AtomicUsize::new(0));
    let count = Arc::clone(&seen);
    let consumer = thread::spawn(move || {
        let deadline = Instant::now() + Duration::from_secs(5);
        while count.load(Ordering::Relaxed) < 20 && Instant::now() < deadline {
            match receiver.recv_timeout(Duration::from_millis(20)) {
                Ok(_) => {
                    count.fetch_add(1, Ordering::Relaxed);
                }
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            }
        }
    });

    let producer_domain = domain.clone();
    assert_completes("fast-consumer producer", move || {
        for index in 0..20 {
            assert!(put(&producer_domain, index, index as u8));
        }
    });
    wait_until(|| seen.load(Ordering::Relaxed) == 20);
    assert_eq!(domain.subscriber_drops().unwrap(), 0);
    let snapshot = domain.entity_snapshot(ENTITY).unwrap();
    assert_eq!(snapshot.len(), 20);

    consumer.join().unwrap();
    dispatcher.shutdown();
}

#[test]
fn slow_consumer_does_not_block_domain_operations() {
    let (domain, receiver) = harness();
    let dispatcher = Dispatcher::start(&domain);
    let (held_tx, held_rx) = sync_channel(0);
    let (release_tx, release_rx) = sync_channel(0);
    let consumer = thread::spawn(move || {
        let _ = receiver
            .recv_timeout(Duration::from_secs(5))
            .expect("first fact");
        held_tx.send(()).unwrap();
        release_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("release");
        while receiver.try_recv().is_ok() {}
    });

    let first_domain = domain.clone();
    assert_completes("slow-consumer first put", move || {
        assert!(put(&first_domain, 0, 0));
    });
    held_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("consumer holds first fact");

    let producer_domain = domain.clone();
    assert_completes("slow-consumer producer", move || {
        for index in 1..80 {
            assert!(put(&producer_domain, index, index as u8));
        }
        assert_eq!(producer_domain.entity_snapshot(ENTITY).unwrap().len(), 80);
    });

    release_tx.send(()).unwrap();
    consumer.join().unwrap();
    dispatcher.shutdown();
}

#[test]
fn stopped_consumer_overflows_and_recovers() {
    let (domain, receiver) = harness();
    let dispatcher = Dispatcher::start(&domain);

    let producer_domain = domain.clone();
    assert_completes("stopped-consumer producer", move || {
        for index in 0..80 {
            assert!(put(&producer_domain, index, index as u8));
        }
    });
    wait_until(|| domain.subscriber_drops().unwrap() > 0);
    assert_eq!(domain.entity_snapshot(ENTITY).unwrap().len(), 80);

    let mut drained = 0;
    while receiver.try_recv().is_ok() {
        drained += 1;
    }
    assert!(drained > 0);

    let recovery_domain = domain.clone();
    assert_completes("post-recovery put", move || {
        assert!(put(&recovery_domain, 999, 0xaa));
    });
    let expected_key = 999u64.to_le_bytes();
    loop {
        let fact = receiver
            .recv_timeout(Duration::from_secs(5))
            .expect("post-recovery fact");
        assert_eq!(fact.entity, ENTITY);
        if fact.key() == expected_key {
            break;
        }
    }

    let snapshot = domain.entity_snapshot(ENTITY).unwrap();
    assert_eq!(snapshot.len(), 81);
    let mut record = [0u8; 8];
    domain
        .entity_get(ENTITY, &999u64.to_le_bytes(), &mut record)
        .unwrap();
    assert_eq!(record, [0xaa; 8]);

    dispatcher.shutdown();
}
