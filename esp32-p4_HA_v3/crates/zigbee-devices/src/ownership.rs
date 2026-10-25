//! Ownership contract between profiles, vendor handlers and standard ZCL.
//!
//! This is a build/runtime decision table, not a second registry. Claims are
//! deliberately small and bounded; the caller stores them next to a device.

pub const MAX_CLAIMS: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Owner {
    Standard,
    Profile,
    Vendor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClaimKind {
    Incoming,
    Output,
    Configure,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClaimKey {
    pub endpoint: u8,
    pub cluster: u16,
    pub attribute: Option<u16>,
    pub command: Option<u8>,
}

impl ClaimKey {
    pub const fn attribute(endpoint: u8, cluster: u16, attribute: u16) -> Self {
        Self {
            endpoint,
            cluster,
            attribute: Some(attribute),
            command: None,
        }
    }

    pub const fn command(endpoint: u8, cluster: u16, command: u8) -> Self {
        Self {
            endpoint,
            cluster,
            attribute: None,
            command: Some(command),
        }
    }

    pub const fn configure(endpoint: u8, cluster: u16, attribute: u16) -> Self {
        Self {
            endpoint,
            cluster,
            attribute: Some(attribute),
            command: None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Claim {
    pub kind: ClaimKind,
    pub key: ClaimKey,
    pub owner: Owner,
    /// Vendor/profile may suppress the standard owner without emitting a value.
    pub suppresses_standard: bool,
}

impl Claim {
    pub const fn new(kind: ClaimKind, key: ClaimKey, owner: Owner) -> Self {
        Self {
            kind,
            key,
            owner,
            suppresses_standard: false,
        }
    }

    pub const fn suppress(kind: ClaimKind, key: ClaimKey) -> Self {
        Self {
            kind,
            key,
            owner: Owner::Vendor,
            suppresses_standard: true,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Resolution {
    None,
    Owned(Owner),
    Suppressed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Claims {
    claims: [Option<Claim>; MAX_CLAIMS],
    count: u8,
}

impl Default for Claims {
    fn default() -> Self {
        Self::new()
    }
}

impl Claims {
    pub const fn new() -> Self {
        Self {
            claims: [None; MAX_CLAIMS],
            count: 0,
        }
    }

    pub fn push(&mut self, claim: Claim) -> bool {
        if self.claims[..usize::from(self.count)]
            .iter()
            .flatten()
            .any(|old| *old == claim)
        {
            return true;
        }
        let index = usize::from(self.count);
        if index == MAX_CLAIMS {
            return false;
        }
        self.claims[index] = Some(claim);
        self.count += 1;
        true
    }

    pub fn resolve(&self, kind: ClaimKind, key: ClaimKey) -> Resolution {
        let mut best: Option<&Claim> = None;
        let mut suppressed = false;
        for claim in self.claims[..usize::from(self.count)].iter().flatten() {
            if claim.kind != kind || claim.key != key {
                continue;
            }
            if claim.suppresses_standard {
                suppressed = true;
            }
            if best.map_or(true, |old| claim.owner > old.owner) {
                best = Some(claim);
            }
        }
        if suppressed {
            Resolution::Suppressed
        } else {
            best.map_or(Resolution::None, |claim| Resolution::Owned(claim.owner))
        }
    }

    pub fn len(&self) -> usize {
        usize::from(self.count)
    }
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vendor_overrides_profile_and_standard() {
        let key = ClaimKey::attribute(1, 0x0006, 0x0000);
        let mut claims = Claims::new();
        assert!(claims.push(Claim::new(ClaimKind::Incoming, key, Owner::Standard)));
        assert!(claims.push(Claim::new(ClaimKind::Incoming, key, Owner::Profile)));
        assert!(claims.push(Claim::new(ClaimKind::Incoming, key, Owner::Vendor)));
        assert_eq!(
            claims.resolve(ClaimKind::Incoming, key),
            Resolution::Owned(Owner::Vendor)
        );
    }

    #[test]
    fn suppression_works_without_vendor_output() {
        let key = ClaimKey::attribute(1, 0x0006, 0x0000);
        let mut claims = Claims::new();
        claims.push(Claim::new(ClaimKind::Incoming, key, Owner::Standard));
        claims.push(Claim::suppress(ClaimKind::Incoming, key));
        assert_eq!(
            claims.resolve(ClaimKind::Incoming, key),
            Resolution::Suppressed
        );
    }

    #[test]
    fn incoming_attribute_and_output_command_are_distinct() {
        let incoming = ClaimKey::attribute(1, 0x0006, 0x0000);
        let output = ClaimKey::command(1, 0x0006, 0x01);
        let mut claims = Claims::new();
        claims.push(Claim::new(ClaimKind::Incoming, incoming, Owner::Profile));
        claims.push(Claim::new(ClaimKind::Output, output, Owner::Profile));
        assert_eq!(
            claims.resolve(ClaimKind::Incoming, incoming),
            Resolution::Owned(Owner::Profile)
        );
        assert_eq!(
            claims.resolve(ClaimKind::Output, output),
            Resolution::Owned(Owner::Profile)
        );
        assert_eq!(
            claims.resolve(ClaimKind::Incoming, output),
            Resolution::None
        );
        assert_eq!(
            claims.resolve(ClaimKind::Configure, output),
            Resolution::None
        );
    }

    #[test]
    fn duplicate_claims_do_not_grow() {
        let key = ClaimKey::configure(1, 0x0402, 0x0000);
        let mut claims = Claims::new();
        assert!(claims.push(Claim::new(ClaimKind::Configure, key, Owner::Profile)));
        assert!(claims.push(Claim::new(ClaimKind::Configure, key, Owner::Profile)));
        assert_eq!(claims.len(), 1);
    }
}
