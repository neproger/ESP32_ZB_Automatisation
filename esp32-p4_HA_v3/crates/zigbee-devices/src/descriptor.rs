//! Bounded endpoint/cluster discovery model and persisted signature.
//!
//! Mirrors the V2 interview shape: Active Endpoints followed by one Simple
//! Descriptor per endpoint. Input clusters are server-side, output clusters are
//! client-side. No attribute list is implied by this structure.

pub const MAX_ENDPOINTS: usize = 8;
pub const MAX_CLUSTERS: usize = 16;
pub const SIGNATURE_MAX: usize = 512;
const SIGNATURE_MAGIC: u8 = 0xD1;
const SIGNATURE_VERSION: u8 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClusterRole {
    Client,
    Server,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClusterEntry {
    pub cluster_id: u16,
    pub role: ClusterRole,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EndpointDescriptor {
    pub endpoint: u8,
    pub profile_id: u16,
    pub device_id: u16,
    clusters: [ClusterEntry; MAX_CLUSTERS],
    cluster_count: u8,
}

impl EndpointDescriptor {
    pub const fn new(endpoint: u8, profile_id: u16, device_id: u16) -> Self {
        Self {
            endpoint,
            profile_id,
            device_id,
            clusters: [ClusterEntry {
                cluster_id: 0,
                role: ClusterRole::Server,
            }; MAX_CLUSTERS],
            cluster_count: 0,
        }
    }

    pub fn push(&mut self, cluster_id: u16, role: ClusterRole) -> bool {
        let index = usize::from(self.cluster_count);
        if index == MAX_CLUSTERS {
            return false;
        }
        self.clusters[index] = ClusterEntry { cluster_id, role };
        self.cluster_count += 1;
        true
    }

    pub fn clusters(&self) -> &[ClusterEntry] {
        &self.clusters[..usize::from(self.cluster_count)]
    }

    pub fn cluster_count(&self) -> u8 {
        self.cluster_count
    }

    pub fn has_server(&self, cluster_id: u16) -> bool {
        self.clusters().iter().any(|entry| {
            entry.cluster_id == cluster_id && entry.role == ClusterRole::Server
        })
    }

    pub fn has_client(&self, cluster_id: u16) -> bool {
        self.clusters().iter().any(|entry| {
            entry.cluster_id == cluster_id && entry.role == ClusterRole::Client
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceDescriptor {
    endpoints: [Option<EndpointDescriptor>; MAX_ENDPOINTS],
    count: u8,
}

impl Default for DeviceDescriptor {
    fn default() -> Self {
        Self::new()
    }
}

impl DeviceDescriptor {
    pub const fn new() -> Self {
        Self {
            endpoints: [None; MAX_ENDPOINTS],
            count: 0,
        }
    }

    pub fn push(&mut self, endpoint: EndpointDescriptor) -> bool {
        let index = usize::from(self.count);
        if index == MAX_ENDPOINTS {
            return false;
        }
        if self.endpoint(endpoint.endpoint).is_some() {
            return false;
        }
        self.endpoints[index] = Some(endpoint);
        self.count += 1;
        true
    }

    pub fn endpoint(&self, endpoint: u8) -> Option<&EndpointDescriptor> {
        self.endpoints
            .iter()
            .flatten()
            .find(|descriptor| descriptor.endpoint == endpoint)
    }

    pub fn endpoints(&self) -> impl Iterator<Item = &EndpointDescriptor> {
        self.endpoints.iter().flatten()
    }

    pub fn endpoint_count(&self) -> u8 {
        self.count
    }

    pub fn encode(&self, out: &mut [u8]) -> Result<usize, DescriptorError> {
        let needed = 3 + usize::from(self.count) * 6
            + self
                .endpoints()
                .map(|endpoint| usize::from(endpoint.cluster_count) * 3)
                .sum::<usize>();
        if out.len() < needed {
            return Err(DescriptorError::TooLarge);
        }
        out[0] = SIGNATURE_MAGIC;
        out[1] = SIGNATURE_VERSION;
        out[2] = self.count;
        let mut offset = 3;
        for endpoint in self.endpoints() {
            out[offset] = endpoint.endpoint;
            out[offset + 1..offset + 3].copy_from_slice(&endpoint.profile_id.to_le_bytes());
            out[offset + 3..offset + 5].copy_from_slice(&endpoint.device_id.to_le_bytes());
            out[offset + 5] = endpoint.cluster_count;
            offset += 6;
            for cluster in endpoint.clusters() {
                out[offset..offset + 2].copy_from_slice(&cluster.cluster_id.to_le_bytes());
                out[offset + 2] = match cluster.role {
                    ClusterRole::Client => 0,
                    ClusterRole::Server => 1,
                };
                offset += 3;
            }
        }
        Ok(offset)
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, DescriptorError> {
        if bytes.len() < 3 || bytes[0] != SIGNATURE_MAGIC {
            return Err(DescriptorError::BadMagic);
        }
        if bytes[1] != SIGNATURE_VERSION {
            return Err(DescriptorError::UnsupportedVersion(bytes[1]));
        }
        let count = bytes[2];
        if usize::from(count) > MAX_ENDPOINTS {
            return Err(DescriptorError::TooManyEndpoints);
        }
        let mut descriptor = Self::new();
        let mut offset = 3;
        for _ in 0..count {
            if offset + 6 > bytes.len() {
                return Err(DescriptorError::Truncated);
            }
            let endpoint = bytes[offset];
            let profile_id = u16::from_le_bytes([bytes[offset + 1], bytes[offset + 2]]);
            let device_id = u16::from_le_bytes([bytes[offset + 3], bytes[offset + 4]]);
            let clusters = bytes[offset + 5];
            offset += 6;
            if usize::from(clusters) > MAX_CLUSTERS {
                return Err(DescriptorError::TooManyClusters);
            }
            let mut value = EndpointDescriptor::new(endpoint, profile_id, device_id);
            for _ in 0..clusters {
                if offset + 3 > bytes.len() {
                    return Err(DescriptorError::Truncated);
                }
                let cluster_id = u16::from_le_bytes([bytes[offset], bytes[offset + 1]]);
                let role = match bytes[offset + 2] {
                    0 => ClusterRole::Client,
                    1 => ClusterRole::Server,
                    _ => return Err(DescriptorError::BadRole(bytes[offset + 2])),
                };
                if !value.push(cluster_id, role) {
                    return Err(DescriptorError::TooManyClusters);
                }
                offset += 3;
            }
            if !descriptor.push(value) {
                return Err(DescriptorError::DuplicateOrFullEndpoint);
            }
        }
        if offset != bytes.len() {
            return Err(DescriptorError::TrailingBytes);
        }
        Ok(descriptor)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DescriptorError {
    TooLarge,
    BadMagic,
    UnsupportedVersion(u8),
    Truncated,
    TrailingBytes,
    TooManyEndpoints,
    TooManyClusters,
    DuplicateOrFullEndpoint,
    BadRole(u8),
}

#[cfg(test)]
mod tests {
    use super::*;

    fn descriptor() -> DeviceDescriptor {
        let mut first = EndpointDescriptor::new(1, 0x0104, 0x0100);
        assert!(first.push(0x0006, ClusterRole::Server));
        assert!(first.push(0x0008, ClusterRole::Server));
        assert!(first.push(0x0019, ClusterRole::Client));
        let mut second = EndpointDescriptor::new(2, 0x0104, 0x0302);
        assert!(second.push(0x0402, ClusterRole::Server));
        let mut device = DeviceDescriptor::new();
        assert!(device.push(first));
        assert!(device.push(second));
        device
    }

    #[test]
    fn multi_endpoint_roles_roundtrip() {
        let device = descriptor();
        assert_eq!(device.endpoint_count(), 2);
        assert!(device.endpoint(1).unwrap().has_server(0x0006));
        assert!(device.endpoint(1).unwrap().has_client(0x0019));
        assert!(device.endpoint(2).unwrap().has_server(0x0402));
        let mut bytes = [0u8; SIGNATURE_MAX];
        let len = device.encode(&mut bytes).unwrap();
        let decoded = DeviceDescriptor::decode(&bytes[..len]).unwrap();
        assert_eq!(decoded, device);
    }

    #[test]
    fn sequence_matters_and_unknown_endpoint_is_none() {
        let device = descriptor();
        let ids: Vec<u8> = device.endpoints().map(|endpoint| endpoint.endpoint).collect();
        assert_eq!(ids, [1, 2]);
        assert!(device.endpoint(3).is_none());
    }

    #[test]
    fn malformed_signatures_are_rejected() {
        assert_eq!(DeviceDescriptor::decode(&[0, 0, 0]), Err(DescriptorError::BadMagic));
        assert_eq!(
            DeviceDescriptor::decode(&[SIGNATURE_MAGIC, 2, 0]),
            Err(DescriptorError::UnsupportedVersion(2))
        );
        assert_eq!(
            DeviceDescriptor::decode(&[SIGNATURE_MAGIC, SIGNATURE_VERSION, 9]),
            Err(DescriptorError::TooManyEndpoints)
        );
        assert_eq!(
            DeviceDescriptor::decode(&[SIGNATURE_MAGIC, SIGNATURE_VERSION, 1]),
            Err(DescriptorError::Truncated)
        );
    }

    #[test]
    fn bounds_and_duplicates_are_explicit() {
        let mut endpoint = EndpointDescriptor::new(1, 0, 0);
        for cluster in 0..MAX_CLUSTERS {
            assert!(endpoint.push(cluster as u16, ClusterRole::Server));
        }
        assert!(!endpoint.push(0xFFFF, ClusterRole::Server));
        let mut device = DeviceDescriptor::new();
        assert!(device.push(endpoint));
        assert!(!device.push(endpoint));
    }
}