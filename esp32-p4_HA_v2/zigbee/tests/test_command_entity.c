#include "semantics/ha_zigbee.h"
#include "ha_model/ha_properties.h"
#include "semantics/semantics.h"
#include "zigbee/zigbee_entity_binding.h"

#include <stdio.h>
#include <string.h>

/*
 * A3.2 (docs/STEP_A_PLAN.md): command-by-entity. Adapter резолвит entity_id → (uid, endpoint)
 * по binding (reverse), затем мост строит физическую команду. Здесь проверяем обе половины,
 * которые executor соединяет (без Domain/очереди).
 */

#define CAP 4

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_reverse_lookup_round_trip(void)
{
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;
    zb_entity_binding_t reg[CAP];
    memset(reg, 0, sizeof(reg));
    size_t n = 0;

    ha_entity_id_t id1 = HA_ENTITY_ID_NONE;
    ha_entity_id_t id2 = HA_ENTITY_ID_NONE;
    CHECK(zb_entity_resolve(reg, CAP, &n, uid, 1, &id1, NULL));
    CHECK(zb_entity_resolve(reg, CAP, &n, uid, 2, &id2, NULL));

    ha_device_uid_t back_uid = 0;
    uint8_t back_ep = 0;
    CHECK(zb_entity_binding_find_physical(reg, n, id1, &back_uid, &back_ep));
    CHECK(back_uid == uid && back_ep == 1);
    CHECK(zb_entity_binding_find_physical(reg, n, id2, &back_uid, &back_ep));
    CHECK(back_uid == uid && back_ep == 2);

    CHECK(!zb_entity_binding_find_physical(reg, n, 0xDEADBEEFull, &back_uid, &back_ep));
}

static void test_build_command_for_resolved_entity(void)
{
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;
    zb_entity_binding_t reg[CAP];
    memset(reg, 0, sizeof(reg));
    size_t n = 0;
    ha_entity_id_t id = HA_ENTITY_ID_NONE;
    CHECK(zb_entity_resolve(reg, CAP, &n, uid, 2, &id, NULL));

    ha_device_uid_t target_uid = 0;
    uint8_t target_ep = 0;
    CHECK(zb_entity_binding_find_physical(reg, n, id, &target_uid, &target_ep));

    const ha_zb_state_key_t target = {.device_uid = target_uid, .endpoint = target_ep};
    ha_zb_command_t command = {0};
    CHECK(semantics_build_command(&target, HA_PROPERTY_POWER, HA_ACTION_TOGGLE, NULL, &command));
    CHECK(command.device_uid == uid);
    CHECK(command.dst_endpoint == 2);
    CHECK(command.cluster_id == HA_ZB_CLUSTER_ON_OFF);
    CHECK(command.command_id == HA_ZB_CMD_ON_OFF_TOGGLE);

    /* Неизвестная entity не резолвится → команду строить не над чем. */
    CHECK(!zb_entity_binding_find_physical(reg, n, 0xABCDEFull, &target_uid, &target_ep));
}

int main(void)
{
    test_reverse_lookup_round_trip();
    test_build_command_for_resolved_entity();

    if (g_failures == 0) {
        printf("all command-entity tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
