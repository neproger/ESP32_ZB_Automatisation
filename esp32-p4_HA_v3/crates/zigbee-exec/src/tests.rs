    use super::fake::FakeTransport;
    use super::*;

    const SHORT: u16 = 0x1234;
    const EP: u8 = 1;

    fn zcl(command: u8, expect: Expect, idempotent: bool) -> Request {
        Request {
            device: 0xA4C1_383B_F9D6_2A03,
            short: SHORT,
            endpoint: EP,
            sleepy: false,
            retry: if idempotent {
                RetryPolicy::SAFE
            } else {
                RetryPolicy::NEVER
            },
            expiry_ms: 10_000,
            timeout_ms: 1_000,
            kind: RequestKind::Zcl {
                cluster: 0x0006,
                command,
                payload: Payload::new(&[]).unwrap(),
                expect,
            },
        }
    }

    fn run(exec: &mut Executor, transport: &mut FakeTransport, now: u64) {
        exec.tick(transport, now);
    }

    #[test]
    fn af_confirm_only_finishes_acknowledged() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x01, Expect::None, true), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        assert_eq!(exec.status(id), Some(Status::Sent));
        transport.confirm(id, 7, true);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Acknowledged);
        assert_eq!(exec.status(id), None);
    }

    #[test]
    fn zcl_response_confirms_after_confirm() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 9, true);
        run(&mut exec, &mut transport, 10);
        transport.response(SHORT, EP, 0x0006, 0x01, 9, None, None);
        run(&mut exec, &mut transport, 20);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn response_before_confirm_is_buffered() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        transport.response(SHORT, EP, 0x0006, 0x01, 42, None, None);
        run(&mut exec, &mut transport, 5);
        transport.confirm(id, 42, true);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn confirmed_read_carries_payload() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(read_attr(), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 9, true);
        run(&mut exec, &mut transport, 10);
        transport
            .response_read(SHORT, EP, 0x0000, 0x01, 9, None, b"\x04\x42\x05_SONOFF")
            .unwrap();
        run(&mut exec, &mut transport, 20);
        let result = exec.take_result().unwrap();
        assert_eq!(result.status, Status::Confirmed);
        assert_eq!(result.read.unwrap().as_slice(), b"\x04\x42\x05_SONOFF");
    }

    #[test]
    fn failed_read_has_no_payload() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(read_attr(), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 9, true);
        run(&mut exec, &mut transport, 10);
        transport
            .response_read(SHORT, EP, 0x0000, 0x01, 9, Some(1), b"\x04\x42\x05_SONOFF")
            .unwrap();
        run(&mut exec, &mut transport, 20);
        let result = exec.take_result().unwrap();
        assert_eq!(result.status, Status::Failed);
        assert!(result.read.is_none());
    }

    #[test]
    fn early_read_payload_is_preserved() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(read_attr(), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport
            .response_read(SHORT, EP, 0x0000, 0x01, 42, None, b"\x04\x42\x05_SONOFF")
            .unwrap();
        run(&mut exec, &mut transport, 5);
        transport.confirm(id, 42, true);
        run(&mut exec, &mut transport, 10);
        let result = exec.take_result().unwrap();
        assert_eq!(result.status, Status::Confirmed);
        assert_eq!(result.read.unwrap().as_slice(), b"\x04\x42\x05_SONOFF");
    }

    #[test]
    fn completed_read_keeps_request_context() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(read_attr(), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 9, true);
        run(&mut exec, &mut transport, 10);
        transport
            .response_read(SHORT, EP, 0x0000, 0x01, 9, None, b"\x04\x42\x05_SONOFF")
            .unwrap();
        run(&mut exec, &mut transport, 20);
        let result = exec.take_result().unwrap();
        let context = exec.take_context(result.id).unwrap();
        assert!(matches!(
            context.kind,
            RequestKind::ReadAttributes {
                cluster: 0x0000,
                ..
            }
        ));
        assert_eq!(context.endpoint, EP);
        assert!(exec.take_context(result.id).is_none());
    }
    fn read_attr() -> Request {
        Request {
            device: 0xA4C1_383B_F9D6_2A03,
            short: SHORT,
            endpoint: EP,
            sleepy: false,
            retry: RetryPolicy::SAFE,
            expiry_ms: 10_000,
            timeout_ms: 1_000,
            kind: RequestKind::ReadAttributes {
                cluster: 0x0000,
                attributes: AttributeList::single(0x0004),
            },
        }
    }

    #[test]
    fn wrong_context_and_tsn_are_ignored() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 9, true);
        run(&mut exec, &mut transport, 10);
        transport.response(SHORT, EP, 0x0006, 0x01, 8, None, None); // wrong tsn
        transport.response(SHORT, EP, 0x0001, 0x01, 9, None, None); // wrong cluster
        transport.response(0x9999, EP, 0x0006, 0x01, 9, None, None); // wrong address
        run(&mut exec, &mut transport, 20);
        assert_eq!(exec.status(id), Some(Status::Acknowledged));
        transport.response(SHORT, EP, 0x0006, 0x01, 9, None, None);
        run(&mut exec, &mut transport, 30);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn idempotent_retry_after_timeout() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        run(&mut exec, &mut transport, 1_001);
        assert_eq!(exec.status(id), Some(Status::Queued));
        run(&mut exec, &mut transport, 1_201);
        assert_eq!(transport.sent.len(), 2);
        transport.confirm(id, 3, true);
        run(&mut exec, &mut transport, 1_010);
        assert_eq!(exec.take_result().unwrap().status, Status::Acknowledged);
    }

    #[test]
    fn non_idempotent_command_is_not_retried() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let _id = exec.enqueue(zcl(0x02, Expect::None, false), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        run(&mut exec, &mut transport, 1_001);
        assert_eq!(exec.take_result().unwrap().status, Status::Timeout);
        assert_eq!(transport.sent.len(), 1);
    }

    #[test]
    fn queued_request_expires_before_send() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let first = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        let mut second = zcl(0x01, Expect::None, true);
        second.expiry_ms = 500;
        let second = exec.enqueue(second, 0).unwrap();
        run(&mut exec, &mut transport, 600);
        assert_eq!(exec.take_result().unwrap().status, Status::Expired);
        assert_eq!(exec.status(second), None);
        transport.confirm(first, 1, true);
        run(&mut exec, &mut transport, 610);
        assert_eq!(transport.sent.len(), 1);
    }

    #[test]
    fn per_endpoint_serialization_but_parallel_across_endpoints() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let a = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        let b = exec.enqueue(zcl(0x01, Expect::None, true), 0).unwrap();
        let mut other = zcl(0x00, Expect::None, true);
        other.endpoint = 2;
        other.device = 0xB;
        let c = exec.enqueue(other, 0).unwrap();
        run(&mut exec, &mut transport, 0);
        assert_eq!(transport.sent.len(), 2);
        let sent_ids: Vec<_> = transport.sent.iter().map(|sent| sent.op).collect();
        assert!(sent_ids.contains(&a));
        assert!(sent_ids.contains(&c));
        assert!(!sent_ids.contains(&b));
        transport.confirm(a, 1, true);
        run(&mut exec, &mut transport, 10);
        run(&mut exec, &mut transport, 10);
        assert!(transport.sent.iter().any(|sent| sent.op == b));
    }

    #[test]
    fn zdo_bind_finishes_on_callback_without_tsn() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let request = Request {
            device: 1,
            short: SHORT,
            endpoint: EP,
            sleepy: false,
            retry: RetryPolicy::SAFE,
            expiry_ms: 10_000,
            timeout_ms: 1_000,
            kind: RequestKind::ZdoBind { cluster: 0x0006 },
        };
        let id = exec.enqueue(request, 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.zdo(id, true);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
    }

    #[test]
    fn zdo_leave_finishes_on_callback_without_tsn() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let request = Request {
            device: 1,
            short: SHORT,
            endpoint: 0,
            sleepy: false,
            retry: RetryPolicy::NEVER,
            expiry_ms: 10_000,
            timeout_ms: 1_000,
            kind: RequestKind::ZdoLeave,
        };
        let id = exec.enqueue(request, 0).unwrap();
        run(&mut exec, &mut transport, 0);
        assert!(exec.pending_leave(1));
        transport.zdo(id, true);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Confirmed);
        assert!(!exec.pending_leave(1));
    }

    #[test]
    fn sleepy_parks_without_blocking_other_queues_then_resumes() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let mut sleepy = zcl(0x00, Expect::None, true);
        sleepy.sleepy = true;
        let id = exec.enqueue(sleepy, 0).unwrap();
        let mut other = zcl(0x00, Expect::None, true);
        other.device = 2;
        other.endpoint = 3;
        let other_id = exec.enqueue(other, 0).unwrap();
        run(&mut exec, &mut transport, 0);
        run(&mut exec, &mut transport, 1_001);
        assert_eq!(exec.status(id), Some(Status::Sent));
        assert!(transport.sent.iter().any(|sent| sent.op == other_id));
        exec.device_awake(SHORT);
        run(&mut exec, &mut transport, 1_100);
        assert_eq!(
            transport.sent.iter().filter(|sent| sent.op == id).count(),
            2
        );
        transport.confirm(id, 4, true);
        run(&mut exec, &mut transport, 1_110);
        assert_eq!(exec.take_result().unwrap().status, Status::Acknowledged);
    }

    #[test]
    fn queue_full_and_payload_limits_are_explicit() {
        let mut exec = Executor::new();
        for _ in 0..MAX_OPS {
            exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        }
        assert_eq!(
            exec.enqueue(zcl(0x00, Expect::None, true), 0),
            Err(ExecError::QueueFull)
        );
        assert_eq!(
            Payload::new(&[0; MAX_PAYLOAD + 1]),
            Err(ExecError::PayloadTooLarge)
        );
    }

    #[test]
    fn early_response_buffer_is_bounded() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec
            .enqueue(zcl(0x00, Expect::ZclResponse { command: 0x01 }, true), 0)
            .unwrap();
        run(&mut exec, &mut transport, 0);
        for tsn in 0..(MAX_EARLY as u8 + 3) {
            transport.response(SHORT, EP, 0x0006, 0x01, tsn + 1, None, None);
            run(&mut exec, &mut transport, u64::from(tsn));
        }
        assert_eq!(exec.early_dropped(), 3);
        assert_eq!(exec.status(id), Some(Status::Sent));
    }

    #[test]
    fn confirm_failure_retries_idempotent_and_fails_non_idempotent() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x00, Expect::None, true), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 1, false);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.status(id), Some(Status::Queued));
        run(&mut exec, &mut transport, 210);
        assert_eq!(exec.status(id), Some(Status::Sent));
        assert_eq!(transport.sent.len(), 2);

        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let id = exec.enqueue(zcl(0x02, Expect::None, false), 0).unwrap();
        run(&mut exec, &mut transport, 0);
        transport.confirm(id, 1, false);
        run(&mut exec, &mut transport, 10);
        assert_eq!(exec.take_result().unwrap().status, Status::Failed);
    }
