//! Web transport adapter: translates Web Actions into Domain/config/Device calls.

use ha_domain::canonical::{
    ActionId, CanonicalCommand, CommandValue, EntityId, PropertyId, COMMAND_DISCOVERY,
    COMMAND_PROPERTY, SOURCE_UI,
};
use ha_domain::{DomainHandle, FactMeta, Value};
use ha_web_service::Actions;

pub struct WebActions {
    domain: DomainHandle,
}

impl WebActions {
    /// App-lifetime adapter object passed to the Web transport.
    pub fn new(domain: DomainHandle) -> &'static dyn Actions {
        Box::leak(Box::new(Self { domain }))
    }
}

impl Actions for WebActions {
    fn command(
        &self,
        entity: EntityId,
        property: PropertyId,
        action: ActionId,
        value: CommandValue,
    ) -> Result<(), ha_domain::Error> {
        let command = CanonicalCommand {
            entity,
            property,
            action,
            value,
        };
        self.domain.post(
            COMMAND_PROPERTY,
            &command.encode(),
            None,
            FactMeta {
                source: SOURCE_UI as u8,
                value: Value::NONE,
            },
        )
    }

    fn upsert(&self, entity: u32, key: Vec<u8>, record: Vec<u8>) -> Result<(), ha_domain::Error> {
        ha_config::put_record(&self.domain, SOURCE_UI as u8, entity, &key, &record).map(|_| ())
    }

    fn remove(&self, entity: u32, key: Vec<u8>) -> Result<(), ha_domain::Error> {
        ha_config::remove(&self.domain, entity, &key)
    }

    fn rename_entity(&self, entity: u64, name: String) -> Result<(), ha_domain::Error> {
        self.domain.transaction(|transaction| {
            ha_device::rename_entity(transaction, SOURCE_UI as u8, entity, &name).map(|_| ())
        })
    }

    fn rename_device(&self, device: u64, name: String) -> Result<(), ha_domain::Error> {
        self.domain.transaction(|transaction| {
            ha_device::rename(transaction, SOURCE_UI as u8, device, &name).map(|_| ())
        })
    }

    fn remove_device(&self, device: u64) -> Result<(), ha_domain::Error> {
        ha_device::remove(&self.domain, SOURCE_UI as u8, device).map(|_| ())
    }

    fn open_network(&self, seconds: u8) -> i32 {
        match self.domain.post(
            COMMAND_DISCOVERY,
            &[seconds],
            None,
            FactMeta {
                source: SOURCE_UI as u8,
                value: Value::NONE,
            },
        ) {
            Ok(()) => 0,
            Err(error) => {
                log::warn!("web: discovery rejected: {error:?}");
                -1
            }
        }
    }
}
