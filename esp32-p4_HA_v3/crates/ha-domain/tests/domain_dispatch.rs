use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{sync_channel, Receiver};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use ha_domain::{Backing, Domain, DomainHandle, EntityDesc, Error, Fact, FactMeta, Filter, Value};

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

// Production subscriptions must use the non-blocking channel sink. Callback
// sinks execute under the Domain lock and are suitable only for short work.
// A panic can poison Domain only while dispatch_once holds the guard; this is
// an indirect lock-contract check, not a proof of UI/WS non-blocking.
#[test]
fn callback_sink_runs_under_domain_lock() {
    let mut domain = Domain::new(8, 4, 8).unwrap();
    domain
        .register_entity(EntityDesc {
            entity: ENTITY,
            key_size: 8,
            record_size: 8,
            capacity: 8,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    domain.subscribe(Filter::all(), |_| panic!("callback sink"));
    domain
        .entity_put(ENTITY, &1u64.to_le_bytes(), &[1u8; 8], meta())
        .unwrap();

    let domain = DomainHandle::new(domain);
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _ = domain.dispatch_once();
    }));
    assert!(result.is_err());
    assert_eq!(
        domain.entity_get(ENTITY, &1u64.to_le_bytes(), &mut [0u8; 8]),
        Err(Error::InvalidState)
    );
}
