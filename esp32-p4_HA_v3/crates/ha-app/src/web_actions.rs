use std::sync::Mutex;

use ha_domain::canonical::{
    ActionId, CanonicalCommand, CommandValue, EntityId, PropertyId, COMMAND_DISCOVERY,
    COMMAND_PROPERTY, SOURCE_UI,
};
use ha_domain::{DomainHandle, FactMeta, Value};
use ha_web_service::Actions;
use ha_zigbee_service as zigbee_service;

struct Context {
    domain: DomainHandle,
    zigbee: zigbee_service::Handle,
}

static CONTEXT: Mutex<Option<Context>> = Mutex::new(None);

pub fn init(domain: DomainHandle, zigbee: zigbee_service::Handle) {
    *CONTEXT.lock().unwrap() = Some(Context { domain, zigbee });
}

fn with<T>(
    action: impl FnOnce(&Context) -> Result<T, ha_domain::Error>,
) -> Result<T, ha_domain::Error> {
    let guard = CONTEXT.lock().unwrap();
    let Some(context) = guard.as_ref() else {
        return Err(ha_domain::Error::InvalidState);
    };
    action(context)
}

pub struct WebActions;

impl Actions for WebActions {
    fn command(
        &self,
        entity: EntityId,
        property: PropertyId,
        action: ActionId,
        value: CommandValue,
    ) -> Result<(), ha_domain::Error> {
        with(|context| {
            let command = CanonicalCommand {
                entity,
                property,
                action,
                value,
            };
            context.domain.post(
                COMMAND_PROPERTY,
                &command.encode(),
                None,
                FactMeta {
                    source: SOURCE_UI as u8,
                    value: Value::NONE,
                },
            )
        })
    }

    fn upsert(&self, entity: u32, key: Vec<u8>, record: Vec<u8>) -> Result<(), ha_domain::Error> {
        with(|context| {
            ha_config::put_record(&context.domain, SOURCE_UI as u8, entity, &key, &record)
                .map(|_| ())
        })
    }

    fn remove(&self, entity: u32, key: Vec<u8>) -> Result<(), ha_domain::Error> {
        with(|context| ha_config::remove(&context.domain, entity, &key))
    }

    fn rename_entity(&self, entity: u64, name: String) -> Result<(), ha_domain::Error> {
        with(|context| {
            context.domain.transaction(|transaction| {
                ha_device::rename_entity(transaction, SOURCE_UI as u8, entity, &name).map(|_| ())
            })
        })
    }

    fn rename_device(&self, device: u64, name: String) -> Result<(), ha_domain::Error> {
        with(|context| {
            context.domain.transaction(|transaction| {
                ha_device::rename(transaction, SOURCE_UI as u8, device, &name).map(|_| ())
            })
        })
    }

    fn remove_device(&self, device: u64) -> Result<(), ha_domain::Error> {
        with(|context| context.zigbee.remove_device(device))
    }

    fn open_network(&self, seconds: u8) -> i32 {
        match with(|context| {
            context.domain.post(
                COMMAND_DISCOVERY,
                &[seconds],
                None,
                FactMeta {
                    source: SOURCE_UI as u8,
                    value: Value::NONE,
                },
            )
        }) {
            Ok(()) => 0,
            Err(error) => {
                log::warn!("web: discovery rejected: {error:?}");
                -1
            }
        }
    }
}

pub static WEB_ACTIONS: WebActions = WebActions;
