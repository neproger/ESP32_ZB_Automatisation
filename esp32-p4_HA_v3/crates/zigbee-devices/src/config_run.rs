//! Sequential configure plan runner over `zigbee-exec`.
//!
//! It knows request shapes, not models: bind/read/report maps to a request and
//! the run waits for its result. One run is active at a time; the HA app queues
//! pending devices outside this type.

use z2m_rs::{ConfigureEndpoint, ConfigurePlan, ConfigureStep};
use zigbee_exec::{
    AttributeList, Executor, OperationResult, Request, RequestKind, RetryPolicy, Status,
};

#[derive(Clone, Copy, Debug)]
pub struct RunOptions {
    pub version: u16,
    pub timeout_ms: u32,
    pub retries: u8,
    /// Standard ZCL discovery: a missing read is normal, not a plan failure.
    pub optional_reads: bool,
}

impl RunOptions {
    pub fn from_plan(plan: &ConfigurePlan) -> Self {
        Self {
            version: plan.version,
            timeout_ms: plan.timeout_ms,
            retries: plan.retries,
            optional_reads: false,
        }
    }
}

pub struct ConfigRun {
    device: (u64, u16),
    endpoint: u8,
    definition: &'static str,
    steps: Vec<ConfigureStep>,
    version: u16,
    timeout_ms: u32,
    retries: u8,
    optional_reads: bool,
    next: usize,
    op: Option<u32>,
    pending: Option<OperationResult>,
    failed: bool,
    done: bool,
}

impl ConfigRun {
    pub fn from_plan(
        exec: &mut Executor,
        device: (u64, u16),
        endpoint: u8,
        definition: &'static str,
        plan: &'static ConfigurePlan,
        now: u64,
    ) -> Self {
        Self::new(
            exec,
            device,
            endpoint,
            definition,
            plan.steps,
            RunOptions::from_plan(plan),
            now,
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub fn new(
        exec: &mut Executor,
        device: (u64, u16),
        endpoint: u8,
        definition: &'static str,
        steps: &[ConfigureStep],
        options: RunOptions,
        now: u64,
    ) -> Self {
        let mut run = Self {
            device,
            endpoint,
            definition,
            steps: steps.to_vec(),
            version: options.version,
            timeout_ms: options.timeout_ms,
            retries: options.retries,
            optional_reads: options.optional_reads,
            next: 0,
            op: None,
            pending: None,
            failed: false,
            done: false,
        };
        run.schedule(exec, now);
        run
    }

    pub fn eui(&self) -> u64 {
        self.device.0
    }

    pub fn endpoint(&self) -> u8 {
        self.endpoint
    }

    pub fn definition(&self) -> &'static str {
        self.definition
    }

    pub fn version(&self) -> u16 {
        self.version
    }

    pub fn current_step(&self) -> Option<ConfigureStep> {
        self.steps.get(self.next).copied()
    }

    pub fn accepts(&self, operation: u32) -> bool {
        self.op == Some(operation)
    }

    pub fn failed(&self) -> bool {
        self.failed
    }

    pub fn on_result(&mut self, result: OperationResult) {
        if self.op == Some(result.id) {
            self.pending = Some(result);
        }
    }

    /// Returns true when the plan is finished (done or failed).
    pub fn tick(&mut self, exec: &mut Executor, now: u64) -> bool {
        if self.done || self.failed {
            return true;
        }
        if let Some(result) = self.pending.take() {
            match result.status {
                Status::Confirmed | Status::Acknowledged => {
                    self.next += 1;
                    self.op = None;
                }
                other
                    if self.optional_reads
                        && matches!(self.current_step(), Some(ConfigureStep::Read { .. })) =>
                {
                    log::info!(
                        "configure: optional read step={} skipped: {other:?}",
                        self.next
                    );
                    self.next += 1;
                    self.op = None;
                }
                other => {
                    self.failed = true;
                    log::warn!("configure: step={} failed: {other:?}", self.next);
                    return true;
                }
            }
        }
        if self.next >= self.steps.len() {
            self.done = true;
            log::info!("configure: done version={} (RAM)", self.version);
            return true;
        }
        if self.op.is_none() {
            self.schedule(exec, now);
        }
        self.done || self.failed
    }

    fn schedule(&mut self, exec: &mut Executor, now: u64) {
        let Some((endpoint, kind)) = self.kind() else {
            self.failed = true;
            log::warn!("configure: unsupported step={}", self.next);
            return;
        };
        let request = Request {
            device: self.device.0,
            short: self.device.1,
            endpoint,
            // Battery devices must not fail a plan because they slept.
            sleepy: true,
            retry: RetryPolicy {
                max_attempts: self.retries.clamp(1, 3),
            },
            expiry_ms: self.timeout_ms,
            timeout_ms: 3_000,
            kind,
        };
        match exec.enqueue(request, now) {
            Ok(id) => {
                self.op = Some(id);
                log::info!("configure: step={} op={id}", self.next);
            }
            Err(error) => {
                self.failed = true;
                log::warn!("configure: enqueue step={} failed: {error:?}", self.next);
            }
        }
    }

    fn kind(&self) -> Option<(u8, RequestKind)> {
        let step = self.steps.get(self.next)?;
        let endpoint = match step {
            ConfigureStep::Read { endpoint, .. }
            | ConfigureStep::Bind { endpoint, .. }
            | ConfigureStep::Report { endpoint, .. } => match endpoint {
                ConfigureEndpoint::Id(id) => *id,
                ConfigureEndpoint::First => self.endpoint,
            },
        };
        let kind = match step {
            ConfigureStep::Read {
                cluster,
                attributes,
                ..
            } => RequestKind::ReadAttributes {
                cluster: *cluster,
                attributes: AttributeList::new(attributes).ok()?,
            },
            ConfigureStep::Bind { cluster, .. } => RequestKind::ZdoBind { cluster: *cluster },
            ConfigureStep::Report {
                cluster,
                attribute,
                attribute_type,
                min,
                max,
                change,
                ..
            } => RequestKind::ConfigureReporting {
                cluster: *cluster,
                attribute: *attribute,
                attribute_type: *attribute_type,
                min: *min,
                max: *max,
                change: (*change).min(u32::from(u8::MAX)) as u8,
            },
        };
        Some((endpoint, kind))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use zigbee_exec::fake::FakeTransport;

    const DEVICE: u64 = 0x0012_4b00_0a1b_2c3d;
    const SHORT: u16 = 0x1234;
    const EP: u8 = 1;

    fn report_step() -> ConfigureStep {
        ConfigureStep::Report {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0001,
            attribute: 0x0021,
            attribute_type: 0x20,
            min: 3600,
            max: 3600,
            change: 10,
        }
    }

    fn options(optional_reads: bool) -> RunOptions {
        RunOptions {
            version: 1,
            timeout_ms: 5_000,
            retries: 2,
            optional_reads,
        }
    }

    fn confirm(exec: &mut Executor, transport: &mut FakeTransport, op: u32, tsn: u8, now: u64) {
        transport.confirm(op, tsn, true);
        exec.tick(transport, now);
    }

    #[test]
    fn report_plan_completes_after_confirmed_response() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let mut run = ConfigRun::new(
            &mut exec,
            (DEVICE, SHORT),
            EP,
            "test",
            &[report_step()],
            options(false),
            0,
        );
        exec.tick(&mut transport, 0);
        let op = transport.last().unwrap().op;
        confirm(&mut exec, &mut transport, op, 9, 10);
        transport.response(SHORT, EP, 0x0001, 0x07, 9, None, Some(0));
        exec.tick(&mut transport, 20);
        while let Some(result) = exec.take_result() {
            run.on_result(result);
        }
        assert!(run.tick(&mut exec, 30));
        assert!(!run.failed());
    }

    #[test]
    fn optional_read_timeout_does_not_fail_the_plan() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let steps = &[ConfigureStep::Read {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
            attributes: &[0x0000],
        }];
        let mut run = ConfigRun::new(
            &mut exec,
            (DEVICE, SHORT),
            EP,
            "standard",
            steps,
            options(true),
            0,
        );
        exec.tick(&mut transport, 0);
        let op = transport.last().unwrap().op;
        run.on_result(OperationResult {
            id: op,
            status: Status::Timeout,
            read: None,
            zdo: None,
        });
        assert!(run.tick(&mut exec, 10));
        assert!(!run.failed());
    }

    #[test]
    fn non_optional_read_failure_fails_the_plan() {
        let mut exec = Executor::new();
        let mut transport = FakeTransport::new();
        let steps = &[ConfigureStep::Read {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0006,
            attributes: &[0x0000],
        }];
        let mut run = ConfigRun::new(
            &mut exec,
            (DEVICE, SHORT),
            EP,
            "test",
            steps,
            options(false),
            0,
        );
        exec.tick(&mut transport, 0);
        let op = transport.last().unwrap().op;
        run.on_result(OperationResult {
            id: op,
            status: Status::Failed,
            read: None,
            zdo: None,
        });
        assert!(run.tick(&mut exec, 10));
        assert!(run.failed());
    }

    #[test]
    fn too_many_read_attributes_fail_the_step_without_panicking() {
        // MAX_READ_ATTRIBUTES is 8; a larger list must fail the step, not panic.
        let mut exec = Executor::new();
        let steps = &[ConfigureStep::Read {
            endpoint: ConfigureEndpoint::First,
            cluster: 0x0000,
            attributes: &[0, 1, 2, 3, 4, 5, 6, 7, 8],
        }];
        let mut run = ConfigRun::new(
            &mut exec,
            (DEVICE, SHORT),
            EP,
            "test",
            steps,
            options(false),
            0,
        );
        assert!(run.tick(&mut exec, 0));
        assert!(run.failed());
    }
}
