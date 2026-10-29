use ha_domain::canonical::{
    CommandValue, ACTION_ON, ACTION_SET, ACTION_TOGGLE, PROPERTY_BRIGHTNESS, PROPERTY_POWER,
};
use ha_domain::command::CommandCore;
use ha_domain::{Error, Value as DomainValue};
use z2m_rs::capabilities::{DIM_CT_EXPOSES, DIM_CT_TO};
use z2m_rs::{Fingerprint, Manufacturer, Profile, Support};
use zigbee_devices::{update_binding_short, Binding, CommandRouter, Confirmed};
use zigbee_exec::fake::FakeTransport;
use zigbee_exec::{Executor, RequestKind};

const ENTITY: u64 = 0x1122_3344_5566_7788;
const UID: u64 = 0x0012_4B00_0A1B_2C3D;
const SHORT: u16 = 0x2001;

static PROFILE: Profile = Profile {
    id: "test.router",
    vendor: "test",
    fingerprints: &[Fingerprint {
        manufacturer: Manufacturer::Any,
        model: "ROUTER",
    }],
    exposes: DIM_CT_EXPOSES,
    from: &[],
    to: DIM_CT_TO,
    configure: None,
    omit_optional_params: false,
    turns_off_at_brightness_1: false,
    support: Support::IDENTIFIED,
};

fn binding(profile: &'static Profile) -> Binding {
    Binding {
        entity: ENTITY,
        eui: UID,
        short: SHORT,
        endpoint: 1,
        profile,
        standard: None,
        confirmed: Confirmed::new(),
    }
}

#[test]
fn submits_by_entity_without_exposing_eui_or_zcl() {
    let mut exec = Executor::new();
    let mut core = CommandCore::new();
    let bindings = [binding(&PROFILE)];
    let mut router = CommandRouter::new(&mut exec, &bindings);
    router
        .submit(
            &mut core,
            ENTITY,
            PROPERTY_BRIGHTNESS,
            ACTION_SET,
            CommandValue::Scalar(DomainValue::f32(50.0)),
            0,
        )
        .unwrap();
    drop(router);

    let mut transport = FakeTransport::new();
    exec.tick(&mut transport, 0);
    let sent = transport.last().expect("request sent");
    let RequestKind::SendZcl {
        cluster,
        command,
        payload,
        ..
    } = sent.request.kind
    else {
        panic!("expected SendZcl");
    };
    assert_eq!((cluster, command), (0x0008, 0x04));
    assert_eq!(payload.as_slice(), [127, 0, 0, 0, 0]);
}

#[test]
fn power_value_selects_on_action() {
    let mut exec = Executor::new();
    let mut core = CommandCore::new();
    let bindings = [binding(&PROFILE)];
    let mut router = CommandRouter::new(&mut exec, &bindings);
    router
        .submit(
            &mut core,
            ENTITY,
            PROPERTY_POWER,
            ACTION_ON,
            CommandValue::Scalar(DomainValue::bool(true)),
            0,
        )
        .unwrap();
    drop(router);

    let mut transport = FakeTransport::new();
    exec.tick(&mut transport, 0);
    let sent = transport.last().expect("request sent");
    let RequestKind::SendZcl {
        cluster, command, ..
    } = sent.request.kind
    else {
        panic!("expected SendZcl");
    };
    assert_eq!((cluster, command), (0x0006, 0x01));
}

#[test]
fn toggle_selects_onoff_toggle_command() {
    let mut exec = Executor::new();
    let mut core = CommandCore::new();
    let bindings = [binding(&PROFILE)];
    let mut router = CommandRouter::new(&mut exec, &bindings);
    router
        .submit(
            &mut core,
            ENTITY,
            PROPERTY_POWER,
            ACTION_TOGGLE,
            CommandValue::None,
            0,
        )
        .unwrap();
    drop(router);

    let mut transport = FakeTransport::new();
    exec.tick(&mut transport, 0);
    let sent = transport.last().expect("request sent");
    let RequestKind::SendZcl {
        cluster,
        command,
        payload,
        ..
    } = sent.request.kind
    else {
        panic!("expected SendZcl");
    };
    assert_eq!((cluster, command), (0x0006, 0x02));
    assert!(payload.as_slice().is_empty());
}

#[test]
fn unknown_entity_is_not_found() {
    let mut exec = Executor::new();
    let mut core = CommandCore::new();
    let bindings = [binding(&PROFILE)];
    let mut router = CommandRouter::new(&mut exec, &bindings);
    assert_eq!(
        router.submit(
            &mut core,
            ENTITY + 1,
            PROPERTY_BRIGHTNESS,
            ACTION_SET,
            CommandValue::Scalar(DomainValue::f32(50.0)),
            0,
        ),
        Err(Error::NotFound)
    );
}

#[test]
fn missing_capability_is_rejected_before_send() {
    let mut exec = Executor::new();
    let mut core = CommandCore::new();
    let bindings = [Binding {
        profile: &z2m_rs::TUYA_TS0041,
        ..binding(&PROFILE)
    }];
    let mut router = CommandRouter::new(&mut exec, &bindings);
    assert_eq!(
        router.submit(
            &mut core,
            ENTITY,
            PROPERTY_BRIGHTNESS,
            ACTION_SET,
            CommandValue::Scalar(DomainValue::f32(50.0)),
            0,
        ),
        Err(Error::InvalidArg)
    );
    assert!(core.is_empty());
}

#[test]
fn full_command_core_is_bounded() {
    let mut exec = Executor::new();
    let mut core = CommandCore::new();
    let mut bindings = [binding(&PROFILE); 9];
    for (index, binding) in bindings.iter_mut().enumerate() {
        binding.entity = ENTITY + index as u64;
    }
    let mut router = CommandRouter::new(&mut exec, &bindings);
    for binding in &bindings[..8] {
        router
            .submit(
                &mut core,
                binding.entity,
                PROPERTY_POWER,
                ACTION_ON,
                CommandValue::Scalar(DomainValue::bool(true)),
                0,
            )
            .unwrap();
    }
    assert_eq!(
        router.submit(
            &mut core,
            bindings[8].entity,
            PROPERTY_POWER,
            ACTION_ON,
            CommandValue::Scalar(DomainValue::bool(true)),
            0,
        ),
        Err(Error::NoSpace)
    );
}
#[test]
fn rejoin_updates_short_without_changing_entity() {
    let mut bindings = [binding(&PROFILE)];
    assert!(update_binding_short(&mut bindings, UID, 0x3002));
    assert_eq!(bindings[0].entity, ENTITY);
    assert_eq!(bindings[0].short, 0x3002);
    assert!(!update_binding_short(&mut bindings, UID + 1, 0x3003));
    assert_eq!(bindings[0].short, 0x3002);
}
