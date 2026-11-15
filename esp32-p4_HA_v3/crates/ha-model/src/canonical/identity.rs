use super::*;
pub const SIPHASH_KEY: [u8; 16] = [
    0x45, 0x53, 0x50, 0x33, 0x32, 0x2d, 0x50, 0x34, 0x2d, 0x48, 0x41, 0x2d, 0x76, 0x32, 0x21, 0x00,
];

fn rotl(x: u64, b: u32) -> u64 {
    x.rotate_left(b)
}

fn sip_round(v: &mut [u64; 4]) {
    v[0] = v[0].wrapping_add(v[1]);
    v[1] = rotl(v[1], 13);
    v[1] ^= v[0];
    v[0] = rotl(v[0], 32);
    v[2] = v[2].wrapping_add(v[3]);
    v[3] = rotl(v[3], 16);
    v[3] ^= v[2];
    v[0] = v[0].wrapping_add(v[3]);
    v[3] = rotl(v[3], 21);
    v[3] ^= v[0];
    v[2] = v[2].wrapping_add(v[1]);
    v[1] = rotl(v[1], 17);
    v[1] ^= v[2];
    v[2] = rotl(v[2], 32);
}

pub fn siphash24(key: &[u8; 16], data: &[u8]) -> u64 {
    let k0 = u64::from_le_bytes(key[..8].try_into().unwrap());
    let k1 = u64::from_le_bytes(key[8..].try_into().unwrap());
    let mut v = [
        k0 ^ 0x736f_6d65_7073_6575,
        k1 ^ 0x646f_7261_6e64_6f6d,
        k0 ^ 0x6c79_6765_6e65_7261,
        k1 ^ 0x7465_6462_7974_6573,
    ];
    let left = data.len() & 7;
    let end = data.len() - left;
    let mut b = (data.len() as u64) << 56;
    for chunk in data[..end].chunks(8) {
        let m = u64::from_le_bytes(chunk.try_into().unwrap());
        v[3] ^= m;
        sip_round(&mut v);
        sip_round(&mut v);
        v[0] ^= m;
    }
    for (i, byte) in data[end..].iter().enumerate() {
        b |= u64::from(*byte) << (8 * i);
    }
    v[3] ^= b;
    sip_round(&mut v);
    sip_round(&mut v);
    v[0] ^= b;
    v[2] ^= 0xff;
    for _ in 0..4 {
        sip_round(&mut v);
    }
    v[0] ^ v[1] ^ v[2] ^ v[3]
}

pub fn entity_id_derive(issuer: IssuerId, seed: &[u8]) -> EntityId {
    if seed.len() > SEED_MAX {
        return ENTITY_ID_NONE;
    }
    let mut data = [0u8; 8 + SEED_MAX];
    data[..4].copy_from_slice(&issuer.to_le_bytes());
    data[4..8].copy_from_slice(&(seed.len() as u32).to_le_bytes());
    data[8..8 + seed.len()].copy_from_slice(seed);
    let id = siphash24(&SIPHASH_KEY, &data[..8 + seed.len()]);
    if id == ENTITY_ID_NONE {
        1
    } else {
        id
    }
}

pub fn device_id_derive(source: SourceId, native_id: &[u8]) -> DeviceId {
    if native_id.is_empty() || native_id.len() > SEED_MAX {
        return DEVICE_ID_NONE;
    }
    entity_id_derive(source, native_id)
}

pub fn zigbee_seed(uid: u64, endpoint: u8) -> [u8; 9] {
    let mut seed = [0u8; 9];
    seed[..8].copy_from_slice(&uid.to_le_bytes());
    seed[8] = endpoint;
    seed
}

pub fn zigbee_entity_id(uid: u64, endpoint: u8) -> EntityId {
    entity_id_derive(ISSUER_ZIGBEE, &zigbee_seed(uid, endpoint))
}
