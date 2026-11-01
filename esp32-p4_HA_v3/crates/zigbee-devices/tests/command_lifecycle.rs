//! End-to-end host lifecycle: canonical registration, command core, executor,
//! and reported state from a device report.

use ha_domain::canonical::{
    CanonicalCommand, CommandValue, ACTION_ON, ACTION_SET, CAPABILITIES_LEN, DEVICE_RECORD_LEN,
    ENTITY_CAPABILITIES, ENTITY_ENTITY, ENTITY_LOGICAL_DEVICE, ENTITY_PROPERTY_STATE,
    ENTITY_RECORD_LEN, PROPERTY_BRIGHTNESS, PROPERTY_COLOR, PROPERTY_COLOR_TEMPERATURE,
    PROPERTY_POWER, SOURCE_ZIGBEE, STATE_KEY_LEN, STATE_RECORD_LEN,
};
use ha_domain::{Backing, Domain, DomainHandle, EntityDesc, Value as DomainValue};
use z2m_rs::capabilities::{
    COLORTEMP_TO, COLOR_XY_TO, DIM_CT_EXPOSES, LEVEL_TO, ONOFF_OFF, ONOFF_ON,
};
use z2m_rs::{properties, Fingerprint, Manufacturer, Profile, Support, Update, Value};
use zigbee_devices::canonical_states;
use zigbee_devices::{status_from_exec, DomainAdapter, ZigbeeSink};
use zigbee_devices::{CommandCore, CommandStatus};
use zigbee_exec::fake::FakeTransport;
use zigbee_exec::{Executor, RequestKind, Status};

const UID: u64 = 0x0012_4B00_0A1B_2C3D;
const SHORT: u16 = 0x2001;

static PROFILE: Profile = Profile {
    id: "test.lifecycle",
    vendor: "test",
    fingerprints: &[Fingerprint {
        manufacturer: Manufacturer::Any,
        model: "LIFECYCLE",
    }],
    exposes: DIM_CT_EXPOSES,
    from: &[],
    to: &[ONOFF_ON, ONOFF_OFF, LEVEL_TO, COLORTEMP_TO, COLOR_XY_TO],
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

fn setup(journal_capacity: usize) -> (DomainHandle, u64) {
    let mut domain = Domain::new(journal_capacity, 8, 64).unwrap();
    for (entity, key_size, record_size, capacity) in [
        (ENTITY_LOGICAL_DEVICE, 8, DEVICE_RECORD_LEN, 16),
        (ENTITY_ENTITY, 8, ENTITY_RECORD_LEN, 16),
        (ENTITY_CAPABILITIES, 8, CAPABILITIES_LEN, 16),
        (ENTITY_PROPERTY_STATE, STATE_KEY_LEN, STATE_RECORD_LEN, 256),
    ] {
        domain
            .register_entity(EntityDesc {
                entity,
                key_size,
                record_size,
                capacity,
                backing: Backing::Ram,
                persist_key: None,
            })
            .unwrap();
    }
    let native = format!("{UID:016x}");
    let desc = ha_device::DeviceDesc {
        source: SOURCE_ZIGBEE,
        native_id: &native,
        parent_native_id: None,
        name: "Test light",
    };
    let device = ha_device::register(&mut domain, SOURCE_ZIGBEE as u8, &desc).unwrap();
    let entity = zigbee_devices::zigbee_entity_id(UID, 1);
    ha_device::ensure_entity(
        &mut domain,
        SOURCE_ZIGBEE as u8,
        device,
        entity,
        "Test light",
    )
    .unwrap();
    let caps = zigbee_devices::profile_capabilities(&PROFILE);
    ha_device::set_capabilities(&mut domain, SOURCE_ZIGBEE as u8, entity, &caps).unwrap();
    (DomainHandle::new(domain), entity)
}

fn power_command(entity: u64) -> CanonicalCommand {
    CanonicalCommand {
        entity,
        property: PROPERTY_POWER,
        action: ACTION_ON,
        value: CommandValue::None,
    }
}

fn submit(exec: &mut Executor, core: &mut CommandCore, command: CanonicalCommand) -> u64 {
    let mut sink = ZigbeeSink {
        executor: exec,
        profile: &PROFILE,
        device: (UID, SHORT),
        endpoint: 1,
    };
    core.submit(&mut sink, command, 0).unwrap()
}

fn confirm_default_response(
    exec: &mut Executor,
    transport: &mut FakeTransport,
    op: u64,
    to_command: u8,
) {
    transport.confirm(op as u32, 9, true);
    exec.tick(transport, 1);
    transport.response(SHORT, 1, 0x0006, 0x0B, 9, Some(to_command), Some(0));
    exec.tick(transport, 2);
}

#[test]
fn confirmed_command_does_not_set_reported_state() {
    let (domain, entity) = setup(32);

    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    let mut core = CommandCore::new();
    let op = submit(&mut exec, &mut core, power_command(entity));
    exec.tick(&mut transport, 0);
    confirm_default_response(&mut exec, &mut transport, op, 0x01);
    let result = exec.take_result().unwrap();
    assert_eq!(result.status, Status::Confirmed);
    assert_eq!(
        status_from_exec(result.status),
        Some(CommandStatus::Acknowledged)
    );
    assert!(core.on_result(result.id as u64, status_from_exec(result.status).unwrap()));
    assert_eq!(
        core.pending(op).unwrap().status,
        CommandStatus::Acknowledged
    );
    // Default Response is not reported state.
    assert!(DomainAdapter::read_state(&domain, entity, PROPERTY_POWER).is_none());

    let (states, _) = canonical_states(
        entity,
        &Update::Property {
            property: properties::STATE,
            value: Value::Bool(true),
        },
    );
    let state = states[0].unwrap();
    assert!(
        DomainAdapter::write_state(&domain, state.entity, state.property, state.value).unwrap()
    );
    assert_eq!(
        DomainAdapter::read_state(&domain, entity, PROPERTY_POWER),
        Some(DomainValue::bool(true))
    );
    core.on_reported(entity, PROPERTY_POWER);
    assert!(core.is_empty());
}

#[test]
fn acknowledged_without_report_leaves_reported_absent() {
    let (domain, entity) = setup(16);

    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    let mut core = CommandCore::new();
    let op = submit(&mut exec, &mut core, power_command(entity));
    exec.tick(&mut transport, 0);
    confirm_default_response(&mut exec, &mut transport, op, 0x01);
    let result = exec.take_result().unwrap();
    core.on_result(result.id as u64, status_from_exec(result.status).unwrap());
    assert!(DomainAdapter::read_state(&domain, entity, PROPERTY_POWER).is_none());
    assert_eq!(core.len(), 1);
}

#[test]
fn report_without_command_updates_domain() {
    let (domain, entity) = setup(16);
    let core = CommandCore::new();

    let (states, _) = canonical_states(
        entity,
        &Update::Property {
            property: properties::STATE,
            value: Value::Bool(true),
        },
    );
    let state = states[0].unwrap();
    DomainAdapter::write_state(&domain, state.entity, state.property, state.value).unwrap();
    assert_eq!(
        DomainAdapter::read_state(&domain, entity, PROPERTY_POWER),
        Some(DomainValue::bool(true))
    );
    assert!(core.is_empty());
}

#[test]
fn timeout_fails_operation_without_reported_change() {
    let (domain, entity) = setup(16);

    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    let mut core = CommandCore::new();
    let op = submit(
        &mut exec,
        &mut core,
        CanonicalCommand {
            entity,
            property: PROPERTY_BRIGHTNESS,
            action: ACTION_SET,
            value: CommandValue::Scalar(DomainValue::f32(50.0)),
        },
    );
    exec.tick(&mut transport, 0);
    exec.tick(&mut transport, 10_001);
    let mut failed = false;
    while let Some(result) = exec.take_result() {
        if result.id as u64 == op {
            failed = status_from_exec(result.status) == Some(CommandStatus::Failed);
            core.on_result(result.id as u64, status_from_exec(result.status).unwrap());
        }
    }
    assert!(failed);
    assert!(core.is_empty());
    assert!(DomainAdapter::read_state(&domain, entity, PROPERTY_BRIGHTNESS).is_none());
}
fn send_one(command: CanonicalCommand) -> (u16, u8, Vec<u8>) {
    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    let mut core = CommandCore::new();
    let _ = submit(&mut exec, &mut core, command);
    exec.tick(&mut transport, 0);
    let sent = transport.last().cloned().expect("request sent");
    let request = sent.request;
    let RequestKind::SendZcl {
        cluster,
        command,
        payload,
        ..
    } = request.kind
    else {
        panic!("expected SendZcl");
    };
    (cluster, command, payload.as_slice().to_vec())
}

#[test]
fn brightness_ct_xy_use_generic_zcl_send() {
    let (cluster, command, payload) = send_one(CanonicalCommand {
        entity: 1,
        property: PROPERTY_BRIGHTNESS,
        action: ACTION_SET,
        value: CommandValue::Scalar(DomainValue::f32(50.0)),
    });
    assert_eq!((cluster, command), (0x0008, 0x04));
    assert_eq!(payload, [127, 0, 0, 0, 0]);

    let (cluster, command, payload) = send_one(CanonicalCommand {
        entity: 1,
        property: PROPERTY_COLOR_TEMPERATURE,
        action: ACTION_SET,
        value: CommandValue::Scalar(DomainValue::f32(4000.0)),
    });
    assert_eq!((cluster, command), (0x0300, 0x0A));
    assert_eq!(payload, [250, 0, 0, 0, 0, 0]);

    let (cluster, command, payload) = send_one(CanonicalCommand {
        entity: 1,
        property: PROPERTY_COLOR,
        action: ACTION_SET,
        value: CommandValue::Xy { x: 0.5, y: 0.25 },
    });
    assert_eq!((cluster, command), (0x0300, 0x07));
    assert_eq!(payload, [0x00, 0x80, 0x00, 0x40, 0, 0, 0, 0]);
}
