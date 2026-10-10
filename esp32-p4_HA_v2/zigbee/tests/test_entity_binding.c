#include "zigbee/zigbee_entity_binding.h"

#include <stdio.h>

/*
 * A0.2: Zigbee-private binding lookups (physical ↔ entity). Derivation entity_id — A0.3.
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

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
    CHECK(zb_entity_binding_find_entity(list, count, B, 1, &id) && id == 0x1002);

    ha_device_uid_t uid = 0;
    uint8_t ep = 0;
    CHECK(zb_entity_binding_find_physical(list, count, 0x1000, &uid, &ep) && uid == A && ep == 1);
    CHECK(zb_entity_binding_find_physical(list, count, 0x1002, &uid, &ep) && uid == B && ep == 1);

    /* нет привязки */
    CHECK(!zb_entity_binding_find_entity(list, count, A, 9, &id));
    CHECK(!zb_entity_binding_find_physical(list, count, 0x9999, &uid, &ep));

    /* NULL-guards */
    CHECK(!zb_entity_binding_find_entity(NULL, count, A, 1, &id));
    CHECK(!zb_entity_binding_find_entity(list, count, A, 1, NULL));
    CHECK(!zb_entity_binding_find_physical(list, count, 0x1000, NULL, &ep));
}

int main(void)
{
    test_lookup();

    if (g_failures == 0) {
        printf("all entity-binding tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
