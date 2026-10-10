#include "ha_model/ha_entity.h"

#include <stddef.h>
#include <stdio.h>

/*
 * A0.1 + A0.3 (docs/STEP_A_PLAN.md): transport-agnostic Entity contract и
 * детерминированный вывод entity_id (SipHash-2-4, fixed project key).
 */

_Static_assert(sizeof(ha_entity_id_t) == 8, "entity id ABI");
_Static_assert(sizeof(ha_entity_key_t) == 8, "entity key ABI");
_Static_assert(offsetof(ha_entity_key_t, id) == 0, "entity key layout");
_Static_assert(HA_ENTITY_ID_NONE == 0, "entity none sentinel");
_Static_assert(HA_ENTITY_ISSUER_ZIGBEE == 1, "issuer ABI");

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_key(void)
{
    ha_entity_key_t key = {.id = 0x00124B000A1B2C3Dull};
    CHECK(key.id == 0x00124B000A1B2C3Dull);
}

/* Официальный SipHash-2-4 тест-вектор (key=00..0f, msg=00..0e) → 0xa129ca6149be45e5. */
static void test_siphash_vector(void)
{
    uint8_t key[16];
    uint8_t msg[15];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 15; i++) msg[i] = (uint8_t)i;
    CHECK(ha_siphash24(key, msg, 15) == 0xa129ca6149be45e5ull);
}

static void zigbee_seed(uint64_t uid, uint8_t ep, uint8_t out[9])
{
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(uid >> (8 * i));
    out[8] = ep;
}

static void test_derive(void)
{
    const uint64_t UID = 0x00124B000A1B2C3Dull;
    uint8_t s1[9], s2[9];
    zigbee_seed(UID, 1, s1);
    zigbee_seed(UID, 2, s2);

    const ha_entity_id_t a = ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, s1, 9);
    const ha_entity_id_t b = ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, s1, 9);

    /* детерминизм + не NONE */
    CHECK(a == b);
    CHECK(a != HA_ENTITY_ID_NONE);

    /* другой endpoint → другой id */
    CHECK(a != ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, s2, 9));
    /* другой issuer → другой id */
    CHECK(a != ha_entity_id_derive(HA_ENTITY_ISSUER_SYSTEM, s1, 9));

    /* too-long seed → NONE */
    uint8_t big[HA_ENTITY_ID_DERIVE_SEED_MAX + 1] = {0};
    CHECK(ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, big, sizeof(big)) == HA_ENTITY_ID_NONE);
    CHECK(ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, NULL, 1) == HA_ENTITY_ID_NONE);

    /* golden: ABI-lock композиции контракта (key + LE32(issuer)||LE32(len)||seed) */
    CHECK(a == 0xdf803cbaf588876dull);
}

int main(void)
{
    test_key();
    test_siphash_vector();
    test_derive();

    if (g_failures == 0) {
        printf("all entity tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
