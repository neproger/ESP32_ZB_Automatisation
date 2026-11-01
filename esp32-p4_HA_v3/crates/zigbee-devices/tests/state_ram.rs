use ha_domain::canonical::{
    ENTITY_PROPERTY_STATE, PROPERTY_POWER, STATE_KEY_LEN, STATE_RECORD_LEN,
};
use ha_domain::{Backing, Domain, DomainHandle, EntityDesc, Value};
use zigbee_devices::DomainAdapter;

fn handle(state_capacity: usize) -> DomainHandle {
    let mut domain = Domain::new(8, 4, 16).unwrap();
    domain
        .register_entity(EntityDesc {
            entity: ENTITY_PROPERTY_STATE,
            key_size: STATE_KEY_LEN,
            record_size: STATE_RECORD_LEN,
            capacity: state_capacity,
            backing: Backing::Ram,
            persist_key: None,
        })
        .unwrap();
    DomainHandle::new(domain)
}

#[test]
fn property_state_is_ram_only() {
    let entity = 0x1122_3344_5566_7788;
    let domain = handle(8);
    assert!(
        DomainAdapter::write_state(&domain, entity, PROPERTY_POWER, Value::bool(true)).unwrap()
    );
    assert_eq!(
        DomainAdapter::read_state(&domain, entity, PROPERTY_POWER),
        Some(Value::bool(true))
    );
}

#[test]
fn property_state_uses_the_v2_capacity_budget() {
    let domain = handle(256);

    for index in 0..256u64 {
        DomainAdapter::write_state(&domain, index + 1, PROPERTY_POWER, Value::bool(true)).unwrap();
    }
    assert_eq!(
        DomainAdapter::write_state(&domain, 257, PROPERTY_POWER, Value::bool(true)),
        Err(ha_domain::Error::NoSpace)
    );
}
