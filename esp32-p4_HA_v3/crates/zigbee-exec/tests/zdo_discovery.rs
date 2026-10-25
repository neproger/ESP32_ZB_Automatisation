use zigbee_exec::fake::FakeTransport;
use zigbee_exec::{
    ClusterList, EndpointList, Event, Executor, Request, RequestKind, RetryPolicy,
    SimpleDescriptor, Status, ZdoData,
};

fn request(kind: RequestKind) -> Request {
    Request { device: 1, short: 0x1234, endpoint: 1, sleepy: false, retry: RetryPolicy::SAFE, expiry_ms: 10_000, timeout_ms: 1_000, kind }
}

#[test]
fn zdo_active_endpoints_and_simple_descriptor_reach_result() {
    let mut exec = Executor::new();
    let mut transport = FakeTransport::new();
    let active = exec.enqueue(request(RequestKind::ZdoActiveEndpoints), 0).unwrap();
    exec.tick(&mut transport, 0);
    let mut endpoints = EndpointList::new();
    endpoints.push(1);
    endpoints.push(2);
    transport.push_event(Event::ZdoActiveEndpoints { op: active, endpoints });
    exec.tick(&mut transport, 1);
    let result = exec.take_result().unwrap();
    assert_eq!(result.status, Status::Confirmed);
    assert_eq!(result.zdo, Some(ZdoData::ActiveEndpoints(endpoints)));
    let simple = exec.enqueue(request(RequestKind::ZdoSimpleDescriptor { endpoint: 2 }), 2).unwrap();
    exec.tick(&mut transport, 2);
    let mut server = ClusterList::new();
    server.push(0x0402);
    let descriptor = SimpleDescriptor { endpoint: 2, profile_id: 0x0104, device_id: 0x0302, server_clusters: server, client_clusters: ClusterList::new() };
    transport.push_event(Event::ZdoSimpleDescriptor { op: simple, descriptor });
    exec.tick(&mut transport, 3);
    assert_eq!(exec.take_result().unwrap().zdo, Some(ZdoData::SimpleDescriptor(descriptor)));
}