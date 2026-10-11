#include "ha_model/ha_logical_device.h"

#include "ha_model/ha_entities.h"

#include <stddef.h>

#define TEST_SOURCE ((ha_source_id_t)1)
#include <stdio.h>
#include <string.h>

/*
 * B1' (docs/STEP_B_PLAN.md): формы Logical Device (source/native_id/parent/name) и единая
 * generic derivation. Только форма и чистая функция; без Zigbee.
 */

_Static_assert(sizeof(ha_logical_device_key_t) == 8, "logical device key ABI");
_Static_assert(offsetof(ha_logical_device_key_t, id) == 0, "logical device key layout");
_Static_assert(sizeof(ha_logical_device_record_t) == 80, "logical device record ABI");
_Static_assert(offsetof(ha_logical_device_record_t, parent_device_id) == 0, "record parent layout");
_Static_assert(offsetof(ha_logical_device_record_t, source) == 8, "record source layout");
_Static_assert(offsetof(ha_logical_device_record_t, native_id) == 12, "record native layout");
_Static_assert(offsetof(ha_logical_device_record_t, name) == 44, "record name layout");
_Static_assert(sizeof(ha_device_desc_t) == 100, "device desc ABI");
_Static_assert(HA_DEVICE_ID_NONE == 0, "device id none sentinel");
_Static_assert(HA_ENTITY_LOGICAL_DEVICE == 17, "logical device entity type ABI");
_Static_assert(HA_ENTITY_LOGICAL_DEVICE != HA_ENTITY_ENTITY, "not a generic Entity");

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_record_holds_fields(void)
{
    ha_logical_device_record_t rec = {0};
    rec.parent_device_id = HA_DEVICE_ID_NONE;
    rec.source = TEST_SOURCE;
    memcpy(rec.native_id, "00124b000a1b2c3d", 16);
    memcpy(rec.name, "wall switch", 11);

    CHECK(rec.parent_device_id == HA_DEVICE_ID_NONE);
    CHECK(rec.source == TEST_SOURCE);
    CHECK(strcmp(rec.native_id, "00124b000a1b2c3d") == 0);
    CHECK(strcmp(rec.name, "wall switch") == 0);
}

/* Детерминированный POD: {0} даёт все нулевые байты (нет неявного padding). */
static void test_record_zero_is_all_zero(void)
{
    ha_logical_device_record_t rec = {0};
    const uint8_t *bytes = (const uint8_t *)&rec;
    for (size_t i = 0; i < sizeof(rec); i++) {
        CHECK(bytes[i] == 0);
    }
}

static void test_desc_zero_is_all_zero(void)
{
    ha_device_desc_t desc = {0};
    const uint8_t *bytes = (const uint8_t *)&desc;
    for (size_t i = 0; i < sizeof(desc); i++) {
        CHECK(bytes[i] == 0);
    }
}

static void test_derive_deterministic(void)
{
    const ha_device_id_t a = ha_device_id_derive(TEST_SOURCE, "00124b000a1b2c3d");
    const ha_device_id_t b = ha_device_id_derive(TEST_SOURCE, "00124b000a1b2c3d");
    CHECK(a != HA_DEVICE_ID_NONE);
    CHECK(a == b);
    /* golden: ABI-lock композиции (source + native_id) через ha_entity_id_derive */
    CHECK(a == 0x674a30cbcb8cf157ull);
}

static void test_derive_sensitivity(void)
{
    const ha_device_id_t zigbee = ha_device_id_derive(TEST_SOURCE, "00124b000a1b2c3d");
    CHECK(zigbee != ha_device_id_derive(TEST_SOURCE, "00124b000a1b2c3e")); /* другой native */
    CHECK(zigbee != ha_device_id_derive(((ha_source_id_t)2), "00124b000a1b2c3d")); /* другой source */
    CHECK(zigbee != HA_DEVICE_ID_NONE);
}

static void test_derive_invalid(void)
{
    CHECK(ha_device_id_derive(TEST_SOURCE, NULL) == HA_DEVICE_ID_NONE);
    CHECK(ha_device_id_derive(TEST_SOURCE, "") == HA_DEVICE_ID_NONE);
    /* длиннее seed-лимита derivation (HA_ENTITY_ID_DERIVE_SEED_MAX = 64) */
    char big[72];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(ha_device_id_derive(TEST_SOURCE, big) == HA_DEVICE_ID_NONE);
}

int main(void)
{
    test_record_holds_fields();
    test_record_zero_is_all_zero();
    test_desc_zero_is_all_zero();
    test_derive_deterministic();
    test_derive_sensitivity();
    test_derive_invalid();

    if (g_failures == 0) {
        printf("all logical-device tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
