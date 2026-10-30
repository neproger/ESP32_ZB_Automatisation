use ha_domain::canonical::{CommandValue, ACTION_ON, PROPERTY_POWER};
use ha_domain::Value as DomainValue;
use zigbee_devices::CommandCore;
use zigbee_devices::{CommandRouter, EndpointDescriptor, InterviewManager, STANDARD_PROFILE};
use zigbee_exec::fake::FakeTransport;
use zigbee_exec::{
    ClusterList, EndpointList, Event, Executor, Payload, RequestKind, SimpleDescriptor,
};

fn payload(attribute: u16, text: &str) -> Payload {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&attribute.to_le_bytes());
    bytes.push(0x42);
    bytes.push(text.len() as u8);
    bytes.extend_from_slice(text.as_bytes());
    Payload::new(&bytes).unwrap()
}

fn serve_basic(
    exec: &mut Executor,
    transport: &mut FakeTransport,
    manager: &mut InterviewManager,
    eui: u64,
    now: u64,
    attribute: u16,
    text: &str,
) {
    let sent = transport
        .sent
        .iter()
        .rev()
        .find(|sent| {
            sent.request.device == eui
                && matches!(
                    sent.request.kind,
                    RequestKind::ReadAttributes { attributes, .. }
                        if attributes.as_slice() == [attribute]
                )
        })
        .cloned()
        .expect("basic read request");
    let tsn = (sent.op % 30) as u8 + 1;
    transport.confirm(sent.op, tsn, true);
    exec.tick(transport, now);
    transport
        .response_read(
            sent.request.short,
            1,
            0x0000,
            0x01,
            tsn,
            None,
            payload(attribute, text).as_slice(),
        )
        .unwrap();
    exec.tick(transport, now + 1);
    while let Some(result) = exec.take_result() {
        manager.on_result(exec, result, now + 1);
    }
    exec.tick(transport, now + 2);
}

#[test]
fn discovery_then_standard_command() {
    let eui = 0x0012_4b00_0000_3000;
    let mut manager = InterviewManager::new();
    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    assert!(manager.start(&mut exec, eui, 0x3000, false, 0));
    exec.tick(&mut transport, 0);
    serve_basic(
        &mut exec,
        &mut transport,
        &mut manager,
        eui,
        1,
        0x0004,
        "Other",
    );
    serve_basic(
        &mut exec,
        &mut transport,
        &mut manager,
        eui,
        3,
        0x0005,
        "UNKNOWN",
    );

    let active = transport.last().expect("active ep").op;
    let mut endpoints = EndpointList::new();
    endpoints.push(1);
    transport.push_event(Event::ZdoActiveEndpoints {
        op: active,
        endpoints,
    });
    exec.tick(&mut transport, 5);
    while let Some(result) = exec.take_result() {
        assert!(manager.on_result(&mut exec, result, 5).is_none());
    }
    exec.tick(&mut transport, 6);

    let simple = transport.last().expect("simple desc").op;
    let mut server = ClusterList::new();
    server.push(0x0006);
    transport.push_event(Event::ZdoSimpleDescriptor {
        op: simple,
        descriptor: SimpleDescriptor {
            endpoint: 1,
            profile_id: 0x0104,
            device_id: 0x0000,
            server_clusters: server,
            client_clusters: ClusterList::new(),
        },
    });
    exec.tick(&mut transport, 7);
    let identified = exec
        .take_result()
        .and_then(|result| manager.on_result(&mut exec, result, 7))
        .expect("standard device");
    assert_eq!(identified.profile.id, zigbee_devices::STANDARD_PROFILE_ID);
    assert!(identified
        .descriptor
        .endpoint(1)
        .unwrap()
        .has_server(0x0006));
}

#[test]
fn discovery_keeps_every_endpoint() {
    let eui = 0x0012_4b00_0000_3100;
    let mut manager = InterviewManager::new();
    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    assert!(manager.start(&mut exec, eui, 0x3100, false, 0));
    exec.tick(&mut transport, 0);
    serve_basic(
        &mut exec,
        &mut transport,
        &mut manager,
        eui,
        1,
        0x0004,
        "Other",
    );
    serve_basic(
        &mut exec,
        &mut transport,
        &mut manager,
        eui,
        3,
        0x0005,
        "UNKNOWN",
    );

    let active = transport.last().expect("active ep").op;
    let mut endpoints = EndpointList::new();
    endpoints.push(1);
    endpoints.push(2);
    transport.push_event(Event::ZdoActiveEndpoints {
        op: active,
        endpoints,
    });
    exec.tick(&mut transport, 5);
    while let Some(result) = exec.take_result() {
        assert!(manager.on_result(&mut exec, result, 5).is_none());
    }
    exec.tick(&mut transport, 6);

    let mut onoff = ClusterList::new();
    onoff.push(0x0006);
    let simple = transport.last().expect("simple desc 1").op;
    transport.push_event(Event::ZdoSimpleDescriptor {
        op: simple,
        descriptor: SimpleDescriptor {
            endpoint: 1,
            profile_id: 0x0104,
            device_id: 0x0000,
            server_clusters: onoff,
            client_clusters: ClusterList::new(),
        },
    });
    exec.tick(&mut transport, 7);
    while let Some(result) = exec.take_result() {
        assert!(manager.on_result(&mut exec, result, 7).is_none());
    }
    exec.tick(&mut transport, 8);

    let mut temperature = ClusterList::new();
    temperature.push(0x0402);
    let simple = transport.last().expect("simple desc 2").op;
    transport.push_event(Event::ZdoSimpleDescriptor {
        op: simple,
        descriptor: SimpleDescriptor {
            endpoint: 2,
            profile_id: 0x0104,
            device_id: 0x0302,
            server_clusters: temperature,
            client_clusters: ClusterList::new(),
        },
    });
    exec.tick(&mut transport, 9);
    let identified = exec
        .take_result()
        .and_then(|result| manager.on_result(&mut exec, result, 9))
        .expect("standard device");
    assert_eq!(identified.descriptor.endpoint_count(), 2);
    assert!(identified
        .descriptor
        .endpoint(1)
        .unwrap()
        .has_server(0x0006));
    assert!(identified
        .descriptor
        .endpoint(2)
        .unwrap()
        .has_server(0x0402));
}
#[test]
fn standard_profile_commands_are_capability_gated() {
    let mut descriptor = EndpointDescriptor::new(1, 0x0104, 0x0000);
    assert!(descriptor.push(0x0006, zigbee_devices::ClusterRole::Server));
    let mut exec = Executor::new();
    let mut core = CommandCore::new();
    let binding = zigbee_devices::Binding {
        entity: 1,
        eui: 1,
        short: 0x3000,
        endpoint: 1,
        profile: &STANDARD_PROFILE,
        standard: Some(descriptor),
        confirmed: {
            let mut confirmed = zigbee_devices::Confirmed::new();
            assert!(confirmed.confirm(1, 0x0006, 0x0000));
            confirmed
        },
    };
    let bindings = [binding];
    let mut router = CommandRouter::new(&mut exec, &bindings);
    router
        .submit(
            &mut core,
            1,
            PROPERTY_POWER,
            ACTION_ON,
            CommandValue::Scalar(DomainValue::bool(true)),
            0,
        )
        .unwrap();
}
