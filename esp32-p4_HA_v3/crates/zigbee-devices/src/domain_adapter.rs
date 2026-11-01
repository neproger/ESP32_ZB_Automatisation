//! Zigbee projection I/O for reported property state.
//!
//! Canonical lifecycle and generic CRUD belong to the Device/config services;
//! this module only translates Zigbee properties into `PROPERTY_STATE`.

use ha_domain::canonical::{
    EntityId, PropertyId, StateKey, StateRecord, ENTITY_PROPERTY_STATE, SOURCE_ZIGBEE,
};
use ha_domain::{DomainHandle, FactMeta, Value};

pub struct DomainAdapter;

impl DomainAdapter {
    pub fn write_state(
        domain: &DomainHandle,
        entity: EntityId,
        property: PropertyId,
        value: Value,
    ) -> Result<bool, ha_domain::Error> {
        let key = StateKey { entity, property }.to_bytes();
        let record = StateRecord { value }.encode();
        domain.entity_put(
            ENTITY_PROPERTY_STATE,
            &key,
            &record,
            FactMeta {
                source: SOURCE_ZIGBEE as u8,
                value,
            },
        )
    }

    pub fn read_state(
        domain: &DomainHandle,
        entity: EntityId,
        property: PropertyId,
    ) -> Option<Value> {
        let key = StateKey { entity, property }.to_bytes();
        let mut bytes = [0u8; ha_domain::canonical::STATE_RECORD_LEN];
        domain
            .entity_get(ENTITY_PROPERTY_STATE, &key, &mut bytes)
            .ok()?;
        Value::from_bytes(&bytes)
    }
}
