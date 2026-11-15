use std::collections::VecDeque;
    use super::*;

    #[derive(Clone, Debug)]
    pub struct Sent {
        pub op: OperationId,
        pub request: Request,
    }

    #[derive(Default)]
    pub struct FakeTransport {
        pub sent: Vec<Sent>,
        events: VecDeque<Event>,
        pub fail_next: bool,
    }

    impl FakeTransport {
        pub fn new() -> Self {
            Self::default()
        }

        pub fn confirm(&mut self, op: OperationId, tsn: u8, success: bool) {
            self.events.push_back(Event::Confirm { op, tsn, success });
        }

        #[allow(clippy::too_many_arguments)]
        pub fn response(
            &mut self,
            src: u16,
            endpoint: u8,
            cluster: u16,
            command: u8,
            tsn: u8,
            rsp_to_cmd: Option<u8>,
            status: Option<u8>,
        ) {
            self.events.push_back(Event::ZclResponse {
                src,
                endpoint,
                cluster,
                command,
                tsn,
                rsp_to_cmd,
                status,
                read: None,
            });
        }

        /// Read response carrying the attribute bytes the executor must pass on.
        #[allow(clippy::too_many_arguments)]
        pub fn response_read(
            &mut self,
            src: u16,
            endpoint: u8,
            cluster: u16,
            command: u8,
            tsn: u8,
            status: Option<u8>,
            read: &[u8],
        ) -> Result<(), ExecError> {
            let read = Payload::new(read)?;
            self.events.push_back(Event::ZclResponse {
                src,
                endpoint,
                cluster,
                command,
                tsn,
                rsp_to_cmd: None,
                status,
                read: Some(read),
            });
            Ok(())
        }

        pub fn zdo(&mut self, op: OperationId, success: bool) {
            self.events.push_back(Event::ZdoResult { op, success });
        }

        pub fn push_event(&mut self, event: Event) {
            self.events.push_back(event);
        }

        pub fn last(&self) -> Option<&Sent> {
            self.sent.last()
        }
    }

    impl Transport for FakeTransport {
        fn send(&mut self, op: OperationId, request: &Request) -> Result<(), SendError> {
            if self.fail_next {
                self.fail_next = false;
                return Err(SendError::Io);
            }
            self.sent.push(Sent {
                op,
                request: *request,
            });
            Ok(())
        }

        fn poll(&mut self) -> Option<Event> {
            self.events.pop_front()
        }
    }
