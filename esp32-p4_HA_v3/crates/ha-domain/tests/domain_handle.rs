use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Barrier, Mutex};
use std::thread;
use std::time::Duration;

use ha_domain::{
    Backing, Domain, DomainHandle, EntityDesc, Error, FactKind, FactMeta, Filter, Value,
};

fn handle() -> DomainHandle {
    let mut domain = Domain::new(8, 4, 16).unwrap();
    domain
        .register_entity(EntityDesc {
            entity: 7,
            key_size: 8,
            record_size: 8,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    DomainHandle::new(domain)
}

fn meta() -> FactMeta {
    FactMeta {
        source: 1,
        value: Value::NONE,
    }
}

fn meta_value(value: u32) -> FactMeta {
    FactMeta {
        source: 1,
        value: Value::u32(value),
    }
}

#[test]
fn post_publishes_one_command_sent_on_ok() {
    let domain = handle();
    let calls = Arc::new(AtomicUsize::new(0));
    let sink = Arc::clone(&calls);
    domain
        .register_command(1, move |_| {
            sink.fetch_add(1, Ordering::SeqCst);
            Ok(())
        })
        .unwrap();

    let facts = Arc::new(AtomicUsize::new(0));
    let sink = Arc::clone(&facts);
    domain
        .with_mut(|domain| {
            domain.subscribe(Filter::all(), move |fact| {
                if fact.kind == FactKind::CommandSent {
                    sink.fetch_add(1, Ordering::SeqCst);
                }
                true
            })
        })
        .unwrap();

    domain.post(1, &[], None, meta()).unwrap();
    domain.dispatch_once().unwrap();
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    assert_eq!(facts.load(Ordering::SeqCst), 1);
}

#[test]
fn post_error_publishes_no_command_sent() {
    let domain = handle();
    domain
        .register_command(1, |_| Err(Error::InvalidState))
        .unwrap();
    let facts = Arc::new(AtomicUsize::new(0));
    let sink = Arc::clone(&facts);
    domain
        .with_mut(|domain| {
            domain.subscribe(Filter::all(), move |fact| {
                if fact.kind == FactKind::CommandSent {
                    sink.fetch_add(1, Ordering::SeqCst);
                }
                true
            })
        })
        .unwrap();

    assert_eq!(domain.post(1, &[], None, meta()), Err(Error::InvalidState));
    domain.dispatch_once().unwrap();
    assert_eq!(facts.load(Ordering::SeqCst), 0);
}

#[test]
fn executor_runs_without_domain_lock() {
    let domain = handle();
    domain.entity_put(7, &[1u8; 8], &[2u8; 8], meta()).unwrap();
    let nested = domain.clone();
    domain
        .register_command(1, move |_| {
            nested.entity_get(7, &[1u8; 8], &mut [0u8; 8])?;
            Ok(())
        })
        .unwrap();

    domain.post(1, &[], None, meta()).unwrap();
}

#[test]
fn executor_can_mutate_domain() {
    let domain = handle();
    let nested = domain.clone();
    domain
        .register_command(1, move |_| {
            nested.entity_put(7, &[3u8; 8], &[4u8; 8], meta())?;
            Ok(())
        })
        .unwrap();

    domain.post(1, &[], None, meta()).unwrap();
    let mut record = [0u8; 8];
    assert_eq!(
        domain.entity_get(7, &[3u8; 8], &mut record).map(|_| record),
        Ok([4u8; 8])
    );
}

#[test]
fn unknown_command_does_not_run_executor() {
    let domain = handle();
    let calls = Arc::new(AtomicUsize::new(0));
    let sink = Arc::clone(&calls);
    domain
        .register_command(1, move |_| {
            sink.fetch_add(1, Ordering::SeqCst);
            Ok(())
        })
        .unwrap();

    assert_eq!(domain.post(2, &[], None, meta()), Err(Error::NotFound));
    assert_eq!(calls.load(Ordering::SeqCst), 0);
}

#[test]
fn commands_are_serialized() {
    let domain = handle();
    let active = Arc::new(AtomicUsize::new(0));
    let overlap = Arc::new(AtomicBool::new(false));
    let active_sink = Arc::clone(&active);
    let overlap_sink = Arc::clone(&overlap);
    domain
        .register_command(1, move |_| {
            if active_sink.fetch_add(1, Ordering::SeqCst) != 0 {
                overlap_sink.store(true, Ordering::SeqCst);
            }
            thread::sleep(Duration::from_millis(1));
            active_sink.fetch_sub(1, Ordering::SeqCst);
            Ok(())
        })
        .unwrap();

    let barrier = Arc::new(Barrier::new(3));
    let posters: Vec<_> = (0..2)
        .map(|_| {
            let domain = domain.clone();
            let barrier = Arc::clone(&barrier);
            thread::spawn(move || {
                barrier.wait();
                for _ in 0..50 {
                    domain.post(1, &[], None, meta()).unwrap();
                }
            })
        })
        .collect();
    barrier.wait();
    for poster in posters {
        poster.join().unwrap();
    }
    assert!(!overlap.load(Ordering::SeqCst));
}

#[test]
fn concurrent_posters_and_readers_do_not_deadlock() {
    let domain = handle();
    domain.entity_put(7, &[1u8; 8], &[2u8; 8], meta()).unwrap();
    domain.register_command(1, |_| Ok(())).unwrap();

    let barrier = Arc::new(Barrier::new(5));
    let posters: Vec<_> = (0..2)
        .map(|_| {
            let domain = domain.clone();
            let barrier = Arc::clone(&barrier);
            thread::spawn(move || {
                barrier.wait();
                for _ in 0..50 {
                    domain.post(1, &[], None, meta()).unwrap();
                }
            })
        })
        .collect();
    let readers: Vec<_> = (0..2)
        .map(|_| {
            let domain = domain.clone();
            let barrier = Arc::clone(&barrier);
            thread::spawn(move || {
                barrier.wait();
                for _ in 0..50 {
                    domain.entity_get(7, &[1u8; 8], &mut [0u8; 8]).unwrap();
                }
            })
        })
        .collect();
    barrier.wait();
    for poster in posters {
        poster.join().unwrap();
    }
    for reader in readers {
        reader.join().unwrap();
    }
}

#[test]
fn command_sent_follows_executor_order() {
    let domain = handle();
    let executed = Arc::new(Mutex::new(Vec::new()));
    let sink = Arc::clone(&executed);
    domain
        .register_command(1, move |args| {
            sink.lock()
                .unwrap()
                .push(u32::from_le_bytes(args.try_into().unwrap()));
            Ok(())
        })
        .unwrap();
    let published = Arc::new(Mutex::new(Vec::new()));
    let sink = Arc::clone(&published);
    domain
        .with_mut(|domain| {
            domain.subscribe(Filter::all(), move |fact| {
                if fact.kind == FactKind::CommandSent {
                    sink.lock().unwrap().push(fact.value.as_u32().unwrap());
                }
                true
            })
        })
        .unwrap();

    domain
        .post(1, &1u32.to_le_bytes(), None, meta_value(1))
        .unwrap();
    domain
        .post(1, &2u32.to_le_bytes(), None, meta_value(2))
        .unwrap();
    domain.dispatch_once().unwrap();

    assert_eq!(*executed.lock().unwrap(), vec![1, 2]);
    assert_eq!(*published.lock().unwrap(), vec![1, 2]);
}

#[test]
fn executor_panic_poisons_command_lane() {
    let domain = handle();
    domain
        .register_command(1, |_| panic!("executor panic"))
        .unwrap();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        domain.post(1, &[], None, meta())
    }));
    assert!(result.is_err());
    assert_eq!(domain.post(1, &[], None, meta()), Err(Error::InvalidState));
}

#[test]
fn recursive_post_returns_invalid_state() {
    let domain = handle();
    let nested: Arc<Mutex<Option<DomainHandle>>> = Arc::new(Mutex::new(None));
    let nested_handler = Arc::clone(&nested);
    domain
        .register_command(1, move |_| {
            let handle = nested_handler.lock().unwrap();
            handle.as_ref().unwrap().post(1, &[], None, meta())
        })
        .unwrap();
    *nested.lock().unwrap() = Some(domain.clone());

    assert_eq!(domain.post(1, &[], None, meta()), Err(Error::InvalidState));
}

#[test]
fn post_from_with_mut_returns_invalid_state() {
    let domain = handle();
    domain.register_command(1, |_| Ok(())).unwrap();
    let nested = domain.clone();
    let result = domain
        .with_mut(|_| nested.post(1, &[], None, meta()))
        .unwrap();
    assert_eq!(result, Err(Error::InvalidState));
}

#[test]
fn empty_entity_snapshot_is_empty() {
    let domain = handle();
    let snapshot = domain.entity_snapshot(7).unwrap();
    assert_eq!(snapshot.len(), 0);
    assert_eq!(snapshot.key(0), None);
}

#[test]
fn entity_snapshot_is_owned_and_contiguous() {
    let domain = handle();
    domain.entity_put(7, &[1u8; 8], &[2u8; 8], meta()).unwrap();
    domain.entity_put(7, &[3u8; 8], &[4u8; 8], meta()).unwrap();

    let snapshot = domain.entity_snapshot(7).unwrap();
    assert_eq!(snapshot.entity(), 7);
    assert_eq!(snapshot.len(), 2);
    let first = (0..snapshot.len())
        .find(|index| snapshot.key(*index) == Some(&[1u8; 8][..]))
        .unwrap();
    assert_eq!(snapshot.record(first), Some(&[2u8; 8][..]));
    let second = (0..snapshot.len())
        .find(|index| snapshot.key(*index) == Some(&[3u8; 8][..]))
        .unwrap();
    assert_eq!(snapshot.record(second), Some(&[4u8; 8][..]));
    assert_eq!(snapshot.key(2), None);
}

#[test]
fn entity_snapshot_rejects_size_over_limit() {
    let mut domain = Domain::new(8, 4, 16).unwrap();
    domain
        .register_entity(EntityDesc {
            entity: 8,
            key_size: 8,
            record_size: 5000,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    let domain = DomainHandle::new(domain);
    for index in 0..4u8 {
        let mut key = [0u8; 8];
        key[0] = index;
        domain.entity_put(8, &key, &[0u8; 5000], meta()).unwrap();
    }
    assert_eq!(domain.entity_snapshot(8), Err(Error::InvalidSize));
}

#[test]
fn poisoned_domain_returns_invalid_state() {
    let domain = handle();
    let panicking = domain.clone();
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        panicking.with_mut(|_| panic!("poison")).unwrap();
    }));
    assert_eq!(
        domain.entity_get(7, &[0u8; 8], &mut [0u8; 8]),
        Err(Error::InvalidState)
    );
}
