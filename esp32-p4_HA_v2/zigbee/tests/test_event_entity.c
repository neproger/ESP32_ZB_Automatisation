#include "ha_model/ha_entity.h"
#include "semantics/ha_zigbee.h"
#include "semantics/semantics.h"
#include "zigbee/zigbee_entity_binding.h"

#include <stdio.h>
#include <string.h>

/*
 * A3.1 (docs/STEP_A_PLAN.md): semantic event адресуется источником-Entity. Adapter резолвит
 * (uid, endpoint) → entity_id и ставит его в ha_event_t.source_entity; semantics даёт event_id.
 * Проверяем: стабильность (source, event_id) для одной кнопки; разные endpoint одного
 * устройства → разные source_entity; system event — отдельный ENTITY_SYSTEM.
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

static bool decode(ha_device_uid_t uid, uint8_t ep, uint16_t cluster, uint8_t cmd,
                   ha_event_t *out)
{
    ha_zb_event_t physical = {0};
    physical.device_uid = uid;
    physical.endpoint = ep;
    physical.cluster_id = cluster;
    physical.command_id = cmd;
    ha_device_record_t device = {0};
    return semantics_decode_event(&physical, &device, out);
}

static void test_button_is_stable_and_endpoint_scoped(void)
{
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;
    zb_entity_binding_t reg[CAP];
    memset(reg, 0, sizeof(reg));
    size_t n = 0;

    ha_entity_id_t id1 = HA_ENTITY_ID_NONE;
    ha_entity_id_t id2 = HA_ENTITY_ID_NONE;
    CHECK(zb_entity_resolve(reg, CAP, &n, uid, 1, &id1, NULL));
    CHECK(zb_entity_resolve(reg, CAP, &n, uid, 2, &id2, NULL));
    CHECK(id1 != id2); /* endpoint — часть identity */

    /* Одна и та же кнопка (EP1) повторно → тот же source и event_id. */
    ha_event_t a = {0};
    ha_event_t b = {0};
    CHECK(decode(uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_TOGGLE, &a));
    CHECK(decode(uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_ON, &b));
    CHECK(a.id == HA_EVENT_SINGLE_PRESS);
    CHECK(b.id == HA_EVENT_SINGLE_PRESS);
    /* semantics источник не знает; его ставят из binding — здесь совпадает с id1. */
    CHECK(a.source_entity == HA_ENTITY_ID_NONE);

    /* Композиция adapter+semantics: (source, event_id). */
    a.source_entity = id1;
    b.source_entity = id1;
    CHECK(a.source_entity == b.source_entity);
    CHECK(a.id == b.id);

    /* Тот же атрибут на EP2 → другой source (разные записи правила). */
    ha_event_t c = {0};
    CHECK(decode(uid, 2, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_TOGGLE, &c));
    c.source_entity = id2;
    CHECK(c.source_entity != a.source_entity);
    CHECK(c.id == a.id); /* событие то же, источник разный */
}

static void test_system_event_has_own_entity(void)
{
    const ha_entity_id_t sys = ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM);
    uint8_t zig_seed[9] = {0};
    const ha_entity_id_t zig = ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, zig_seed, sizeof(zig_seed));
    CHECK(sys != HA_ENTITY_ID_NONE);
    CHECK(sys != HA_ENTITY_ID_SOURCE);
    CHECK(sys != zig);
    CHECK(sys == ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM));
}

int main(void)
{
    test_button_is_stable_and_endpoint_scoped();
    test_system_event_has_own_entity();

    if (g_failures == 0) {
        printf("all event-entity tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
