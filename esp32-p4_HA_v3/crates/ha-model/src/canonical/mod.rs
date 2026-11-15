//! Canonical HA model, ported from V2 `ha_model` with the same numbers and
//! record layouts. Domain stores these records but does not interpret them.

use crate::{Value, ValueType};

mod command;
mod groups;
mod identity;
mod ids;
mod records;
mod rule;

#[cfg(test)]
mod tests;

pub use command::*;
pub use groups::*;
pub use identity::*;
pub use ids::*;
pub use records::*;
pub use rule::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AbiError {
    InvalidSize,
    InvalidCount,
    NonZeroReserved,
    NonZeroUnused,
    InvalidKind,
}

pub type AbiResult<T> = core::result::Result<T, AbiError>;

fn abi_get_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn abi_get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn abi_get_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
        bytes[offset + 4],
        bytes[offset + 5],
        bytes[offset + 6],
        bytes[offset + 7],
    ])
}

fn abi_put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn abi_put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn abi_put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn abi_require_zero(bytes: &[u8], offset: usize, len: usize) -> AbiResult<()> {
    if bytes[offset..offset + len].iter().any(|byte| *byte != 0) {
        Err(AbiError::NonZeroReserved)
    } else {
        Ok(())
    }
}

fn abi_require_count(value: u8, max: usize) -> AbiResult<()> {
    if usize::from(value) > max {
        Err(AbiError::InvalidCount)
    } else {
        Ok(())
    }
}
