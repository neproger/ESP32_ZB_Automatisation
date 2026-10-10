#include "ha_model/ha_entity.h"

#include <string.h>

/*
 * SipHash-2-4 (reference, public-domain style) + детерминированный вывод entity_id
 * (docs/STEP_A_PLAN.md §3.1). Только стабильные числа и чистая арифметика — host-testable.
 */

#define SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIP_ROUND                        \
    do {                                 \
        v0 += v1;                        \
        v1 = SIP_ROTL(v1, 13);           \
        v1 ^= v0;                        \
        v0 = SIP_ROTL(v0, 32);           \
        v2 += v3;                        \
        v3 = SIP_ROTL(v3, 16);           \
        v3 ^= v2;                        \
        v0 += v3;                        \
        v3 = SIP_ROTL(v3, 21);           \
        v3 ^= v0;                        \
        v2 += v1;                        \
        v1 = SIP_ROTL(v1, 17);           \
        v1 ^= v2;                        \
        v2 = SIP_ROTL(v2, 32);           \
    } while (0)

static uint64_t load_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

uint64_t ha_siphash24(const uint8_t key[16], const void *data, size_t len)
{
    const uint8_t *in = (const uint8_t *)data;
    const uint64_t k0 = load_le64(key);
    const uint64_t k1 = load_le64(key + 8);

    uint64_t v0 = k0 ^ 0x736f6d6570736575ull;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dull;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ull;
    uint64_t v3 = k1 ^ 0x7465646279746573ull;

    const size_t left = len & 7u;
    const size_t end = len - left;
    uint64_t b = ((uint64_t)len) << 56;

    for (size_t i = 0; i < end; i += 8) {
        const uint64_t m = load_le64(in + i);
        v3 ^= m;
        SIP_ROUND;
        SIP_ROUND;
        v0 ^= m;
    }
    for (size_t i = end; i < len; i++) {
        b |= ((uint64_t)in[i]) << (8 * (i - end));
    }
    v3 ^= b;
    SIP_ROUND;
    SIP_ROUND;
    v0 ^= b;
    v2 ^= 0xffu;
    SIP_ROUND;
    SIP_ROUND;
    SIP_ROUND;
    SIP_ROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

/*
 * Фиксированный ключ проекта (не секрет — стабильность derivation между reboot/build).
 * ASCII "ESP32-P4-HA-v2!" + NUL.
 */
static const uint8_t HA_ENTITY_SIPHASH_KEY[16] = {
    0x45, 0x53, 0x50, 0x33, 0x32, 0x2d, 0x50, 0x34,
    0x2d, 0x48, 0x41, 0x2d, 0x76, 0x32, 0x21, 0x00};

ha_entity_id_t ha_entity_id_derive(uint32_t issuer, const void *seed, size_t seed_len)
{
    if (seed_len > HA_ENTITY_ID_DERIVE_SEED_MAX || (seed == NULL && seed_len != 0)) {
        return HA_ENTITY_ID_NONE;
    }
    uint8_t buf[8 + HA_ENTITY_ID_DERIVE_SEED_MAX];
    store_le32(buf, issuer);
    store_le32(buf + 4, (uint32_t)seed_len);
    if (seed_len != 0) {
        memcpy(buf + 8, seed, seed_len);
    }
    const uint64_t id = ha_siphash24(HA_ENTITY_SIPHASH_KEY, buf, 8 + seed_len);
    return (id == HA_ENTITY_ID_NONE) ? (ha_entity_id_t)1 : (ha_entity_id_t)id;
}
