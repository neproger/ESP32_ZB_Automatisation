use super::*;
use crate::entity::FAIL_NEXT_SNAPSHOT;
use std::sync::Arc;
use std::sync::Mutex;

const DEVICE: EntityType = 1;
const OTHER: EntityType = 2;
const SOURCE_ZIGBEE: u8 = 1;
const EUI64: [u8; 8] = [0xa4, 0xc1, 0x38, 0x3b, 0xf9, 0xd6, 0x2a, 0x03];

fn domain(journal: usize) -> Domain {
    Domain::new(journal, 4, 16).unwrap()
}

fn register_device(domain: &mut Domain) {
    domain
        .register_entity(EntityDesc {
            entity: DEVICE,
            key_size: 8,
            record_size: 12,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
}

fn meta(value: Value) -> FactMeta {
    FactMeta {
        source: SOURCE_ZIGBEE,
        value,
    }
}

#[test]
fn snapshot_no_memory_is_atomic_and_recoverable() {
    let mut domain = domain(8);
    register_device(&mut domain);
    let first = [0x11u8; 12];
    domain
        .entity_put(DEVICE, &EUI64, &first, meta(Value::NONE))
        .unwrap();
    let version = domain.entity_meta(DEVICE, &EUI64).unwrap().version;

    FAIL_NEXT_SNAPSHOT.with(|flag| flag.set(true));
    assert_eq!(domain.snapshot(DEVICE), Err(Error::NoMemory));

    let mut unchanged = [0u8; 12];
    domain.entity_get(DEVICE, &EUI64, &mut unchanged).unwrap();
    assert_eq!(unchanged, first);
    assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, version);

    let snapshot = domain.snapshot(DEVICE).unwrap();
    assert_eq!(snapshot.len(), 1);
    assert_eq!(snapshot.key(0), Some(EUI64.as_slice()));
    assert_eq!(snapshot.record(0), Some(first.as_slice()));

    let second = [0x22u8; 12];
    domain
        .entity_put(DEVICE, &[0x55u8; 8], &second, meta(Value::NONE))
        .unwrap();
    assert_eq!(domain.snapshot(DEVICE).unwrap().len(), 2);
}

#[test]
fn injected_clock_sets_fact_time() {
    let mut domain = Domain::new_with_clock(4, 1, 8, || 1_234).unwrap();
    register_device(&mut domain);
    let (sender, receiver) = std::sync::mpsc::sync_channel(4);
    domain.subscribe_channel(Filter::all(), sender);
    domain
        .entity_put(DEVICE, &EUI64, &[0u8; 12], meta(Value::NONE))
        .unwrap();
    domain.dispatch_once();
    assert_eq!(receiver.try_recv().unwrap().ts, 1_234);
}

#[test]
fn registration_validates_geometry_and_duplicates() {
    let mut domain = domain(4);
    register_device(&mut domain);
    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: DEVICE,
            key_size: 8,
            record_size: 12,
            capacity: 4,
            backing: Backing::Ram,
            persist_key: None,
        }),
        Err(Error::AlreadyExists)
    );
    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: 3,
            key_size: FACT_KEY_MAX + 1,
            record_size: 1,
            capacity: 1,
            backing: Backing::Ram,
            persist_key: None,
        }),
        Err(Error::InvalidSize)
    );
    assert_eq!(
        domain.register_entity(EntityDesc {
            entity: 4,
            key_size: 8,
            record_size: 12,
            capacity: 4,
            backing: Backing::Flash,
            persist_key: Some("devices"),
        }),
        Err(Error::InvalidState)
    );
}

#[test]
fn put_get_meta_remove() {
    let mut domain = domain(8);
    register_device(&mut domain);
    let record = [1u8; 12];

    assert_eq!(
        domain.entity_put(DEVICE, &EUI64, &record, meta(Value::NONE)),
        Ok(true)
    );
    assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 1);

    let mut out = [0u8; 12];
    domain.entity_get(DEVICE, &EUI64, &mut out).unwrap();
    assert_eq!(out, record);

    assert_eq!(
        domain.entity_put(DEVICE, &EUI64, &record, meta(Value::NONE)),
        Ok(false)
    );
    assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 1);

    let changed = [2u8; 12];
    assert_eq!(
        domain.entity_put(DEVICE, &EUI64, &changed, meta(Value::NONE)),
        Ok(true)
    );
    assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 2);

    assert_eq!(
        domain.entity_remove(DEVICE, &EUI64, meta(Value::NONE)),
        Ok(())
    );
    assert_eq!(
        domain.entity_get(DEVICE, &EUI64, &mut out),
        Err(Error::NotFound)
    );
    assert_eq!(
        domain.entity_remove(DEVICE, &EUI64, meta(Value::NONE)),
        Err(Error::NotFound)
    );
}

#[test]
fn size_and_source_are_checked_before_mutation() {
    let mut domain = domain(4);
    register_device(&mut domain);
    assert_eq!(
        domain.entity_put(DEVICE, &[0u8; 4], &[0u8; 12], meta(Value::NONE)),
        Err(Error::InvalidSize)
    );
    assert_eq!(
        domain.entity_put(DEVICE, &EUI64, &[0u8; 11], meta(Value::NONE)),
        Err(Error::InvalidSize)
    );
    assert_eq!(
        domain.entity_put(
            DEVICE,
            &EUI64,
            &[0u8; 12],
            FactMeta {
                source: 32,
                value: Value::NONE
            }
        ),
        Err(Error::InvalidArg)
    );
    assert_eq!(
        domain.entity_meta(DEVICE, &EUI64).unwrap_err(),
        Error::NotFound
    );
}

#[test]
fn journal_delivers_only_real_changes_to_matching_subscribers() {
    let mut domain = domain(8);
    register_device(&mut domain);
    domain
        .register_entity(EntityDesc {
            entity: OTHER,
            key_size: 8,
            record_size: 4,
            capacity: 2,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();

    let (sender, receiver) = std::sync::mpsc::sync_channel(4);
    domain.subscribe_channel(
        Filter {
            kind_mask: FactKind::EntityUpserted.bit(),
            ..Filter::entity(DEVICE)
        },
        sender,
    );

    assert!(domain
        .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::NONE))
        .unwrap());
    assert!(!domain
        .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::NONE))
        .unwrap());
    assert!(domain
        .entity_put(OTHER, &[7u8; 8], &[0u8; 4], meta(Value::NONE))
        .unwrap());
    assert!(domain
        .entity_put(DEVICE, &EUI64, &[2u8; 12], meta(Value::NONE))
        .unwrap());

    assert_eq!(domain.dispatch_once(), 2);
    let first = receiver.try_recv().expect("first fact");
    let second = receiver.try_recv().expect("second fact");
    assert_eq!(first.entity, DEVICE);
    assert_eq!(first.key(), EUI64);
    assert_eq!(second.entity, DEVICE);
    assert_eq!(second.key(), EUI64);
    assert_eq!(
        receiver.try_recv(),
        Err(std::sync::mpsc::TryRecvError::Empty)
    );
    assert_eq!(domain.dispatch_once(), 0);
}

#[test]
fn channel_subscription_receives_compact_fact_without_running_subscriber_code() {
    let mut domain = domain(8);
    register_device(&mut domain);
    let (sender, receiver) = std::sync::mpsc::sync_channel(4);
    domain.subscribe_channel(Filter::all(), sender);

    assert!(domain
        .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::bool(true)))
        .unwrap());
    assert_eq!(domain.dispatch_once(), 1);

    let fact = receiver.try_recv().expect("fact in subscriber inbox");
    assert_eq!(fact.entity, DEVICE);
    assert_eq!(fact.key(), EUI64);
    assert_eq!(fact.value, Value::bool(true));
    assert_eq!(domain.dispatch_once(), 0);
}

#[test]
fn dispatch_handle_wakes_the_dispatcher() {
    let mut domain = domain(8);
    register_device(&mut domain);
    let handle = domain.dispatch_handle();
    let (ready_tx, ready_rx) = std::sync::mpsc::channel();
    let waiter = std::thread::spawn(move || {
        handle.wait();
        ready_tx.send(()).unwrap();
    });
    assert!(domain
        .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::bool(true)))
        .unwrap());
    ready_rx
        .recv_timeout(std::time::Duration::from_secs(1))
        .expect("dispatcher wake");
    waiter.join().unwrap();
}

#[test]
fn full_channel_counts_subscriber_drops() {
    let mut domain = domain(8);
    register_device(&mut domain);
    let (sender, _receiver) = std::sync::mpsc::sync_channel(1);
    domain.subscribe_channel(Filter::all(), sender);

    for on in [true, false, true] {
        assert!(domain
            .entity_put(DEVICE, &EUI64, &[u8::from(on); 12], meta(Value::bool(on)),)
            .unwrap());
        domain.dispatch_once();
    }
    assert_eq!(domain.subscriber_drops(), 2);
}

#[test]
fn dispatcher_reports_evicted_facts_as_a_gap() {
    let mut domain = domain(2);
    register_device(&mut domain);
    let (sender, receiver) = std::sync::mpsc::sync_channel(4);
    domain.subscribe_channel(Filter::all(), sender);

    for value in [1u8, 2, 3] {
        let mut record = [0u8; 12];
        record[0] = value;
        domain
            .entity_put(DEVICE, &EUI64, &record, meta(Value::NONE))
            .unwrap();
    }
    assert_eq!(domain.dispatch_once(), 2);
    assert_eq!(receiver.try_recv().unwrap().event_id, 2);
    assert_eq!(receiver.try_recv().unwrap().event_id, 3);
    assert_eq!(domain.gaps(), 1);
}

#[test]
fn payload_ring_publishes_and_evicts() {
    let mut domain = Domain::new(4, 2, 8).unwrap();
    register_device(&mut domain);
    let target = FactTarget {
        entity: DEVICE,
        key: &EUI64,
    };

    assert_eq!(
        domain.payload_put(Some(target), meta(Value::u32(1)), b"abc"),
        Ok(1)
    );
    assert_eq!(
        domain.payload_put(Some(target), meta(Value::u32(2)), b"defg"),
        Ok(2)
    );
    let mut buf = [0u8; 8];
    domain.payload_get(2, &mut buf).unwrap();
    assert_eq!(&buf[..4], b"defg");

    assert_eq!(
        domain.payload_put(Some(target), meta(Value::NONE), b"0123456789"),
        Err(Error::InvalidSize)
    );
    assert_eq!(
        domain.payload_put(Some(target), meta(Value::NONE), b"h"),
        Ok(3)
    );
    assert_eq!(domain.payload_get(1, &mut buf), Err(Error::Stale));
    assert_eq!(domain.payload_get(4, &mut buf), Err(Error::NotFound));
    let mut small = [0u8; 0];
    assert_eq!(domain.payload_get(3, &mut small), Err(Error::InvalidSize));

    let (sender, receiver) = std::sync::mpsc::sync_channel(8);
    domain.subscribe_channel(Filter::kinds(FactKind::Event.bit()), sender);
    assert_eq!(domain.dispatch_once(), 3);
    let events: Vec<_> = std::iter::from_fn(|| receiver.try_recv().ok()).collect();
    assert_eq!(events.len(), 3);
    assert_eq!(
        (
            events[1].payload_ref,
            events[1].payload_size,
            events[1].value
        ),
        (2, 4, Value::u32(2))
    );
    assert_eq!(events[2].payload_ref, 3);
}

#[test]
fn commands_are_handed_off_and_journaled() {
    let mut domain = domain(4);
    register_device(&mut domain);
    let args_seen = Arc::new(Mutex::new(Vec::new()));
    let recorded = Arc::clone(&args_seen);
    domain
        .register_command(7, move |args| {
            recorded.lock().unwrap().extend_from_slice(args);
            Ok(())
        })
        .unwrap();

    let target = FactTarget {
        entity: DEVICE,
        key: &EUI64,
    };
    assert_eq!(
        domain.post(7, &[0xaa, 0x01], Some(target), meta(Value::NONE)),
        Ok(())
    );
    assert_eq!(&*args_seen.lock().unwrap(), &[0xaa, 0x01]);
    assert_eq!(
        domain.post(8, &[], Some(target), meta(Value::NONE)),
        Err(Error::NotFound)
    );

    let (sender, receiver) = std::sync::mpsc::sync_channel(4);
    domain.subscribe_channel(Filter::kinds(FactKind::CommandSent.bit()), sender);
    assert_eq!(domain.dispatch_once(), 1);
    let fact = receiver.try_recv().expect("command fact");
    assert_eq!(fact.kind, FactKind::CommandSent);
    assert_eq!(fact.op, Op::Command);
    assert_eq!(fact.key(), EUI64);
    assert_eq!(
        receiver.try_recv(),
        Err(std::sync::mpsc::TryRecvError::Empty)
    );
}

/// Vertical slice: the exact event captured from TS0041 on hardware
/// (OnOff 0xFD payload 00/01/02 = single/double/hold) reaches the store and
/// produces one delta per real change.
#[test]
fn ts0041_event_becomes_entity_state_and_delta() {
    let mut domain = domain(8);
    register_device(&mut domain);
    let (sender, receiver) = std::sync::mpsc::sync_channel(8);
    domain.subscribe_channel(
        Filter {
            kind_mask: FactKind::EntityUpserted.bit(),
            ..Filter::entity(DEVICE)
        },
        sender,
    );

    let events = [(0x00u8, 0u32), (0x01, 1), (0x02, 2)];
    for (payload, press) in events {
        let mut record = [0u8; 12];
        record[0] = payload;
        let changed = domain
            .entity_put(DEVICE, &EUI64, &record, meta(Value::enum_(press)))
            .unwrap();
        assert!(changed);
    }

    assert_eq!(domain.dispatch_once(), 3);
    let deltas: Vec<_> = std::iter::from_fn(|| receiver.try_recv().ok()).collect();
    assert_eq!(deltas.len(), 3);
    for fact in &deltas {
        assert_eq!(fact.source, SOURCE_ZIGBEE);
        assert_eq!(fact.key(), EUI64);
        assert_eq!(fact.op, Op::EntityPut);
    }
    assert_eq!(
        deltas
            .iter()
            .map(|fact| fact.value.as_enum().unwrap())
            .collect::<Vec<_>>(),
        vec![0, 1, 2]
    );

    let mut record = [0u8; 12];
    domain.entity_get(DEVICE, &EUI64, &mut record).unwrap();
    assert_eq!(record[0], 0x02);
    assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 3);
}
