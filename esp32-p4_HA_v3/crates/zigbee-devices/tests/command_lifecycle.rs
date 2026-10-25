//! End-to-end host lifecycle: canonical registration, command core, executor,
//! and reported state from a device report.

use ha_domain::canonical::{
    CanonicalCommand, CommandValue, ACTION_ON, ACTION_SET, PROPERTY_BRIGHTNESS, PROPERTY_COLOR,
    PROPERTY_COLOR_TEMPERATURE, PROPERTY_POWER,
};
use ha_domain::command::{CommandCore, CommandStatus};
use ha_domain::{Domain, Value as DomainValue};
use z2m_rs::capabilities::{
    COLORTEMP_TO, COLOR_XY_TO, DIM_CT_EXPOSES, LEVEL_TO, ONOFF_OFF, ONOFF_ON,
};
use z2m_rs::{properties, Fingerprint, Manufacturer, Profile, Support, Update, Value};
use zigbee_devices::canonical_states;
use zigbee_devices::{status_from_exec, DomainAdapter, ZigbeeSink};
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
fn registration_is_idempotent_and_power_command_does_not_set_reported() {
    let mut domain = Domain::new(32, 8, 64).unwrap();
    let mut adapter = DomainAdapter::new(&mut domain).unwrap();
    assert!(adapter.ensure_device(UID, "Test light").unwrap().1);
    assert!(!adapter.ensure_device(UID, "Test light").unwrap().1);
    let (entity, changed) = adapter
        .ensure_entity(UID, 1, "Test light", &PROFILE)
        .unwrap();
    assert!(changed);
    let (again, changed) = adapter
        .ensure_entity(UID, 1, "Test light", &PROFILE)
        .unwrap();
    assert_eq!(again, entity);
    assert!(!changed);
    assert!(adapter.read_capabilities(entity).is_some());

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
    assert!(adapter.read_state(entity, PROPERTY_POWER).is_none());

    let (states, _) = canonical_states(
        entity,
        &Update::Property {
            property: properties::STATE,
            value: Value::Bool(true),
        },
    );
    let state = states[0].unwrap();
    assert!(adapter
        .write_state(state.entity, state.property, state.value)
        .unwrap());
    assert_eq!(
        adapter.read_state(entity, PROPERTY_POWER),
        Some(DomainValue::bool(true))
    );
    core.on_reported(entity, PROPERTY_POWER);
    assert!(core.is_empty());
}

#[test]
fn acknowledged_without_report_leaves_reported_absent() {
    let mut domain = Domain::new(16, 8, 64).unwrap();
    let mut adapter = DomainAdapter::new(&mut domain).unwrap();
    adapter.ensure_device(UID, "Test").unwrap();
    let (entity, _) = adapter.ensure_entity(UID, 1, "Test", &PROFILE).unwrap();

    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    let mut core = CommandCore::new();
    let op = submit(&mut exec, &mut core, power_command(entity));
    exec.tick(&mut transport, 0);
    confirm_default_response(&mut exec, &mut transport, op, 0x01);
    let result = exec.take_result().unwrap();
    core.on_result(result.id as u64, status_from_exec(result.status).unwrap());
    assert!(adapter.read_state(entity, PROPERTY_POWER).is_none());
    assert_eq!(core.len(), 1);
}

#[test]
fn report_without_command_updates_domain() {
    let mut domain = Domain::new(16, 8, 64).unwrap();
    let mut adapter = DomainAdapter::new(&mut domain).unwrap();
    adapter.ensure_device(UID, "Test").unwrap();
    let (entity, _) = adapter.ensure_entity(UID, 1, "Test", &PROFILE).unwrap();
    let core = CommandCore::new();

    let (states, _) = canonical_states(
        entity,
        &Update::Property {
            property: properties::STATE,
            value: Value::Bool(true),
        },
    );
    let state = states[0].unwrap();
    adapter
        .write_state(state.entity, state.property, state.value)
        .unwrap();
    assert_eq!(
        adapter.read_state(entity, PROPERTY_POWER),
        Some(DomainValue::bool(true))
    );
    assert!(core.is_empty());
}

#[test]
fn timeout_fails_operation_without_reported_change() {
    let mut domain = Domain::new(16, 8, 64).unwrap();
    let mut adapter = DomainAdapter::new(&mut domain).unwrap();
    adapter.ensure_device(UID, "Test").unwrap();
    let (entity, _) = adapter.ensure_entity(UID, 1, "Test", &PROFILE).unwrap();

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
    assert!(adapter.read_state(entity, PROPERTY_BRIGHTNESS).is_none());
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
