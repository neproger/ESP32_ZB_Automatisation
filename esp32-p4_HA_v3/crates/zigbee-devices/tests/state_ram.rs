use ha_domain::canonical::PROPERTY_POWER;
use ha_domain::{Domain, Value};
use zigbee_devices::{DomainAdapter, RecordSink};

#[derive(Default)]
struct Sink {
    puts: Vec<(u32, Vec<u8>, Vec<u8>)>,
}

impl RecordSink for Sink {
    fn put(&mut self, entity: u32, key: &[u8], record: &[u8]) {
        self.puts.push((entity, key.to_vec(), record.to_vec()));
    }

    fn remove(&mut self, _entity: u32, _key: &[u8]) {}
}

#[test]
fn property_state_is_ram_and_never_reaches_the_sink() {
    let entity = 0x1122_3344_5566_7788;
    let mut domain = Domain::new(8, 4, 16).unwrap();
    let mut sink = Sink::default();
    {
        let mut adapter = DomainAdapter::with_sink(&mut domain, &mut sink).unwrap();
        assert!(adapter
            .write_state(entity, PROPERTY_POWER, Value::bool(true))
            .unwrap());
        assert_eq!(
            adapter.read_state(entity, PROPERTY_POWER),
            Some(Value::bool(true))
        );
    }
    assert!(
        sink.puts.is_empty(),
        "PROPERTY_STATE must stay RAM-only; sink received {:?}",
        sink.puts
    );
}

#[test]
fn property_state_uses_the_v2_capacity_budget() {
    let mut domain = Domain::new(8, 4, 16).unwrap();
    let mut adapter = DomainAdapter::new(&mut domain).unwrap();

    for index in 0..256u64 {
        adapter
            .write_state(index + 1, PROPERTY_POWER, Value::bool(true))
            .unwrap();
    }
    assert_eq!(
        adapter.write_state(257, PROPERTY_POWER, Value::bool(true)),
        Err(ha_domain::Error::NoSpace)
    );
}
