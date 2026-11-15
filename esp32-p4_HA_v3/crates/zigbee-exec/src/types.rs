pub const MAX_OPS: usize = 32;
pub const MAX_PAYLOAD: usize = 64;
pub const MAX_EARLY: usize = 8;
pub const MAX_RESULTS: usize = 32;
pub const MAX_ATTEMPTS: u8 = 3;
pub const EARLY_TTL_MS: u64 = 5_000;
pub const RETRY_BACKOFF_MS: u64 = 200;
pub const MAX_READ_ATTRIBUTES: usize = 8;

/// Per-operation retry budget. Not every idempotent command is safe to repeat
/// (e.g. some Tuya writes); the profile/operation decides the policy.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RetryPolicy {
    pub max_attempts: u8,
}

impl RetryPolicy {
    /// Send once; an unknown outcome is never repeated.
    pub const NEVER: Self = Self { max_attempts: 1 };
    /// Standard safe operations (on/off, read, bind, reporting).
    pub const SAFE: Self = Self { max_attempts: 3 };
}

pub type OperationId = u32;

/// Bounded attribute list for one Read Attributes request.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AttributeList {
    items: [u16; MAX_READ_ATTRIBUTES],
    len: u8,
}

impl AttributeList {
    pub fn new(attributes: &[u16]) -> Result<Self, ExecError> {
        if attributes.is_empty() || attributes.len() > MAX_READ_ATTRIBUTES {
            return Err(ExecError::TooManyAttributes);
        }
        let mut items = [0; MAX_READ_ATTRIBUTES];
        items[..attributes.len()].copy_from_slice(attributes);
        Ok(Self {
            items,
            len: attributes.len() as u8,
        })
    }

    pub fn single(attribute: u16) -> Self {
        let mut items = [0; MAX_READ_ATTRIBUTES];
        items[0] = attribute;
        Self { items, len: 1 }
    }

    pub fn as_slice(&self) -> &[u16] {
        &self.items[..usize::from(self.len)]
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Payload {
    bytes: [u8; MAX_PAYLOAD],
    len: u8,
}

impl Payload {
    pub fn new(data: &[u8]) -> Result<Self, ExecError> {
        if data.len() > MAX_PAYLOAD {
            return Err(ExecError::PayloadTooLarge);
        }
        let mut bytes = [0; MAX_PAYLOAD];
        bytes[..data.len()].copy_from_slice(data);
        Ok(Self {
            bytes,
            len: data.len() as u8,
        })
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.bytes[..usize::from(self.len)]
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Expect {
    /// Delivery confirmation only (AF confirm).
    None,
    /// ZCL response command (e.g. Read Attributes Response 0x01).
    ZclResponse { command: u8 },
    /// Default Response; matched by `rsp_to_cmd`.
    DefaultResponse { to_command: u8 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RequestKind {
    Zcl {
        cluster: u16,
        command: u8,
        payload: Payload,
        expect: Expect,
    },
    /// Read one or more attributes; the response is matched as a normal ZCL response.
    ReadAttributes {
        cluster: u16,
        attributes: AttributeList,
    },
    /// Configure one reporting record; response 0x07 carries the per-record status.
    ConfigureReporting {
        cluster: u16,
        attribute: u16,
        attribute_type: u8,
        min: u16,
        max: u16,
        change: u8,
    },
    /// Generic ZCL command with explicit frame control/manufacturer.
    SendZcl {
        cluster: u16,
        command: u8,
        payload: Payload,
        manufacturer: Option<u16>,
        frame_control: u8,
        expect: Expect,
    },
    /// Completed by the SDK ZDO callback; no TSN waiter is used.
    ZdoBind {
        cluster: u16,
    },
    ZdoActiveEndpoints,
    ZdoSimpleDescriptor {
        endpoint: u8,
    },
    /// ZDO Mgmt_Leave; also completed by the SDK callback.
    ZdoLeave,
}

#[derive(Clone, Copy, Debug)]
pub struct Request {
    /// Durable identity for the queue; current address for correlation.
    pub device: u64,
    pub short: u16,
    pub endpoint: u8,
    pub sleepy: bool,
    pub retry: RetryPolicy,
    pub expiry_ms: u32,
    pub timeout_ms: u32,
    pub kind: RequestKind,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Queued,
    Sent,
    Acknowledged,
    Confirmed,
    Timeout,
    Failed,
    Expired,
}

impl Status {
    pub fn is_final(self) -> bool {
        matches!(
            self,
            Status::Confirmed | Status::Timeout | Status::Failed | Status::Expired
        )
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EndpointList {
    items: [u8; 8],
    len: u8,
}

impl EndpointList {
    pub const fn new() -> Self {
        Self {
            items: [0; 8],
            len: 0,
        }
    }
    pub fn push(&mut self, endpoint: u8) -> bool {
        let index = usize::from(self.len);
        if index == self.items.len() {
            return false;
        }
        self.items[index] = endpoint;
        self.len += 1;
        true
    }
    pub fn as_slice(&self) -> &[u8] {
        &self.items[..usize::from(self.len)]
    }
    pub fn len(&self) -> usize {
        usize::from(self.len)
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl Default for EndpointList {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClusterList {
    items: [u16; 16],
    len: u8,
}

impl ClusterList {
    pub const fn new() -> Self {
        Self {
            items: [0; 16],
            len: 0,
        }
    }
    pub fn push(&mut self, cluster: u16) -> bool {
        let index = usize::from(self.len);
        if index == self.items.len() {
            return false;
        }
        self.items[index] = cluster;
        self.len += 1;
        true
    }
    pub fn as_slice(&self) -> &[u16] {
        &self.items[..usize::from(self.len)]
    }
    pub fn len(&self) -> usize {
        usize::from(self.len)
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl Default for ClusterList {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SimpleDescriptor {
    pub endpoint: u8,
    pub profile_id: u16,
    pub device_id: u16,
    pub server_clusters: ClusterList,
    pub client_clusters: ClusterList,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ZdoData {
    ActiveEndpoints(EndpointList),
    SimpleDescriptor(SimpleDescriptor),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OperationResult {
    pub id: OperationId,
    pub status: Status,
    /// Owned read response bytes; only on `Confirmed` for a read request. A
    /// failed or unmatched response never carries data.
    pub read: Option<Payload>,
    pub zdo: Option<ZdoData>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExecError {
    QueueFull,
    PayloadTooLarge,
    ResultQueueFull,
    TooManyAttributes,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SendError {
    /// Transport is busy; try again on the next tick.
    Full,
    Unsupported,
    Io,
}

#[derive(Clone, Copy, Debug)]
pub enum Event {
    Confirm {
        op: OperationId,
        tsn: u8,
        success: bool,
    },
    ZclResponse {
        src: u16,
        endpoint: u8,
        cluster: u16,
        command: u8,
        tsn: u8,
        rsp_to_cmd: Option<u8>,
        status: Option<u8>,
        /// Bounded read result, attached by the transport only for a matched
        /// successful read; `None` for unrelated/failed records.
        read: Option<Payload>,
    },
    ZdoResult {
        op: OperationId,
        success: bool,
    },
    ZdoActiveEndpoints {
        op: OperationId,
        endpoints: EndpointList,
    },
    ZdoSimpleDescriptor {
        op: OperationId,
        descriptor: SimpleDescriptor,
    },
}

pub trait Transport {
    /// The executor passes `op` as the confirmation context (`user_ctx`).
    fn send(&mut self, op: OperationId, request: &Request) -> Result<(), SendError>;
    fn poll(&mut self) -> Option<Event>;
}

