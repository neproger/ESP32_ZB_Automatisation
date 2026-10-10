#include "ha_model/ha_logical_device.h"

#include "ha_model/ha_entities.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * B1 (docs/STEP_B_PLAN.md): пассивная модель Logical Device. Только форма: ключ по
 * device_id, имя, детерминированный POD-layout, отдельный entity type. Без Zigbee-зависимостей.
 */

_Static_assert(sizeof(ha_logical_device_key_t) == 8, "logical device key ABI");
_Static_assert(offsetof(ha_logical_device_key_t, id) == 0, "logical device key layout");
_Static_assert(sizeof(ha_logical_device_record_t) == HA_LOGICAL_DEVICE_NAME_MAX,
               "logical device record ABI");
_Static_assert(HA_DEVICE_ID_NONE == 0, "device id none sentinel");
_Static_assert(HA_ENTITY_LOGICAL_DEVICE == 17, "logical device entity type ABI");
_Static_assert(HA_ENTITY_LOGICAL_DEVICE != HA_ENTITY_CAPABILITIES, "entity type must be unique");
_Static_assert(HA_ENTITY_LOGICAL_DEVICE != HA_ENTITY_DEVICE, "not the transport device type");
_Static_assert(HA_ENTITY_LOGICAL_DEVICE != HA_ENTITY_ZB_DIAG_STATE, "not the diagnostics state");
_Static_assert(HA_ENTITY_LOGICAL_DEVICE != HA_ENTITY_ENTITY, "not a generic Entity");

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_key_addresses_device_id(void)
{
    const ha_logical_device_key_t key = {.id = 0x00124B000A1B2C3Dull};
    CHECK(key.id == 0x00124B000A1B2C3Dull);
}

static void test_record_holds_name(void)
{
    ha_logical_device_record_t rec = {0};
    memcpy(rec.name, "kitchen", 7);
    CHECK(strcmp(rec.name, "kitchen") == 0);
}

/* Детерминированный POD: {0} даёт все нулевые байты (нет «шумного» padding). */
static void test_zeroed_record_is_all_zero(void)
{
    ha_logical_device_record_t rec = {0};
    const uint8_t *bytes = (const uint8_t *)&rec;
    for (size_t i = 0; i < sizeof(rec); i++) {
        CHECK(bytes[i] == 0);
    }
}

int main(void)
{
    test_key_addresses_device_id();
    test_record_holds_name();
    test_zeroed_record_is_all_zero();

    if (g_failures == 0) {
        printf("all logical-device tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
