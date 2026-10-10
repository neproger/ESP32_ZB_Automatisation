#include "zigbee/zigbee_entity_binding.h"

#include <stdio.h>
#include <string.h>

/*
 * A0.2/A0.4: Zigbee-private binding lookups и resolve-or-create (physical ↔ entity).
 * Derivation entity_id — A0.3 (golden-tested отдельно).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define CAP 8

static void test_seed(void)
{
    uint8_t seed[9];
    CHECK(zb_entity_seed(0x00124B000A1B2C3Dull, 2, seed));
    CHECK(seed[0] == 0x3D && seed[1] == 0x2C && seed[7] == 0x00 && seed[8] == 2);
}

static void test_lookup(void)
{
    const ha_device_uid_t A = 0x00124B000A1B2C3Dull;
    const ha_device_uid_t B = 0x00124B000A1B2C3Eull;
    const zb_entity_binding_t list[] = {
        {.entity_id = 0x1000, .device_uid = A, .endpoint = 1},
        {.entity_id = 0x1001, .device_uid = A, .endpoint = 2},
        {.entity_id = 0x1002, .device_uid = B, .endpoint = 1},
    };
    const size_t count = sizeof(list) / sizeof(list[0]);

    ha_entity_id_t id = 0;
    CHECK(zb_entity_binding_find_entity(list, count, A, 2, &id) && id == 0x1001);
    CHECK(!zb_entity_binding_find_entity(list, count, A, 9, &id));

    ha_device_uid_t uid = 0;
    uint8_t ep = 0;
    CHECK(zb_entity_binding_find_physical(list, count, 0x1002, &uid, &ep) && uid == B && ep == 1);
    CHECK(!zb_entity_binding_find_physical(list, count, 0x9999, &uid, &ep));

    CHECK(!zb_entity_binding_find_entity(NULL, count, A, 1, &id));
}

static void test_resolve(void)
{
    const ha_device_uid_t A = 0x00124B000A1B2C3Dull;
    zb_entity_binding_t reg[CAP] = {0};
    size_t n = 0;

    /* 1. первый interview: binding нет → derived + создан */
    ha_entity_id_t idA1 = 0;
    bool created = false;
    CHECK(zb_entity_resolve(reg, CAP, &n, A, 1, &idA1, &created));
    CHECK(created && n == 1 && idA1 != HA_ENTITY_ID_NONE);

    /* 2. повторный interview: тот же id, не создаётся */
    ha_entity_id_t id2 = 0;
    created = true;
    CHECK(zb_entity_resolve(reg, CAP, &n, A, 1, &id2, &created));
    CHECK(!created && id2 == idA1 && n == 1);

    /* 3. два endpoint одного устройства → разные id */
    ha_entity_id_t idA2 = 0;
    CHECK(zb_entity_resolve(reg, CAP, &n, A, 2, &idA2, &created));
    CHECK(created && idA2 != idA1 && n == 2);

    /* 5. reboot: registry восстановлен из persisted → тот же id, не создаётся */
    created = true;
    CHECK(zb_entity_resolve(reg, CAP, &n, A, 1, &id2, &created));
    CHECK(!created && id2 == idA1);

    /* 6. re-pair тем же EUI64+endpoint → тот же id */
    CHECK(zb_entity_resolve(reg, CAP, &n, A, 1, &id2, &created) && id2 == idA1);

    /* 7. изменившийся endpoint → новый id */
    ha_entity_id_t idA3 = 0;
    CHECK(zb_entity_resolve(reg, CAP, &n, A, 3, &idA3, &created) && created && idA3 != idA1);
}

static void test_order_independent(void)
{
    const ha_device_uid_t A = 0x00124B000A1B2C3Dull;
    zb_entity_binding_t r1[CAP] = {0}, r2[CAP] = {0};
    size_t n1 = 0, n2 = 0;
    ha_entity_id_t x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    CHECK(zb_entity_resolve(r1, CAP, &n1, A, 1, &x1, NULL));
    CHECK(zb_entity_resolve(r1, CAP, &n1, A, 2, &x2, NULL));

    CHECK(zb_entity_resolve(r2, CAP, &n2, A, 2, &y2, NULL)); /* обратный порядок */
    CHECK(zb_entity_resolve(r2, CAP, &n2, A, 1, &y1, NULL));

    CHECK(x1 == y1 && x2 == y2); /* порядок discovery не влияет */
}

static void test_capacity(void)
{
    const ha_device_uid_t A = 0x00124B000A1B2C3Dull;
    zb_entity_binding_t reg[1] = {0};
    size_t n = 0;
    ha_entity_id_t id = 0;
    CHECK(zb_entity_resolve(reg, 1, &n, A, 1, &id, NULL));
    CHECK(!zb_entity_resolve(reg, 1, &n, A, 2, &id, NULL)); /* нет места → false */
}

int main(void)
{
    test_seed();
    test_lookup();
    test_resolve();
    test_order_independent();
    test_capacity();

    if (g_failures == 0) {
        printf("all entity-binding tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
