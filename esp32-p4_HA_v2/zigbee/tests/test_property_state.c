#include "domain/domain.h"
#include "semantics/semantics.h"
#include "zigbee/zigbee_entity_binding.h"
#include "zigbee/zigbee_state.h"

#include <stdio.h>
#include <string.h>

/*
 * A1.3 (docs/STEP_A_PLAN.md): parity raw ↔ canonical. Доказываем на одном входе, что
 * семантическая проекция raw-записи и canonical property state дают одинаковый смысл:
 *   raw:       (uid, ep, cluster, attr) -> semantics -> value
 *   canonical: (entity_id, property_id)            -> value
 * Пока truth — raw; canonical лишь тень (A1.2), переключение — A1.4.
 */

#define CAP 4
#define REG_CAP 4

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

typedef struct {
    domain_t d;
    zb_entity_binding_t reg[REG_CAP];
    size_t n;
} ctx_t;

static void ctx_init(ctx_t *c)
{
    memset(c, 0, sizeof(*c));
    domain_init(&c->d, 4, 16, 8, 64);

    const domain_entity_desc_t device = {
        .type = (domain_entity_t)HA_ENTITY_DEVICE,
        .key_size = sizeof(ha_device_uid_t),
        .payload_size = sizeof(ha_device_record_t),
        .capacity = 4,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    const domain_entity_desc_t raw = {
        .type = (domain_entity_t)HA_ENTITY_ZB_DIAG_STATE,
        .key_size = sizeof(ha_zb_state_key_t),
        .payload_size = sizeof(ha_zb_state_record_t),
        .capacity = 16,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    const domain_entity_desc_t canonical = {
        .type = (domain_entity_t)HA_ENTITY_PROPERTY_STATE,
        .key_size = sizeof(ha_state_key_t),
        .payload_size = sizeof(ha_state_record_t),
        .capacity = 16,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    domain_register_entity(&c->d, &device);
    domain_register_entity(&c->d, &raw);
    domain_register_entity(&c->d, &canonical);
}

/*
 * Прогон одного репорта ровно как в сервисе: raw пишет zigbee_state_apply, затем тень
 * пишется только если endpoint уже имеет Entity и значение имеет смысл.
 */
static void feed(ctx_t *c, ha_device_uid_t uid, uint8_t ep, uint16_t cluster, uint16_t attr,
                 uint8_t zcl_type, uint32_t raw)
{
    const zigbee_report_t report = {
        .device_uid = uid,
        .endpoint = ep,
        .cluster_id = cluster,
        .attr_id = attr,
        .zcl_type = zcl_type,
        .raw = raw,
    };

    bool changed = false;
    CHECK(sys_ok(zigbee_state_apply(&c->d, &report, &changed)));

    ha_entity_id_t entity_id = HA_ENTITY_ID_NONE;
    bool created = false;
    CHECK(zb_entity_resolve(c->reg, REG_CAP, &c->n, uid, ep, &entity_id, &created));

    ha_state_key_t key = {0};
    ha_state_record_t record = {0};
    if (zigbee_property_state_from_report(&report, entity_id, &key, &record)) {
        domain_fact_meta_t meta = {0};
        meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
        bool put_changed = false;
        CHECK(sys_ok(domain_entity_put(&c->d, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &key,
                                       &record, &meta, &put_changed)));
    }
}

static bool value_equal(const ha_value_t *a, const ha_value_t *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case HA_VALUE_BOOL:
        return a->value.b == b->value.b;
    case HA_VALUE_I32:
        return a->value.i32 == b->value.i32;
    case HA_VALUE_U32:
        return a->value.u32 == b->value.u32;
    case HA_VALUE_ENUM:
        return a->value.enum_value == b->value.enum_value;
    case HA_VALUE_FLOAT: {
        float diff = a->value.f32 - b->value.f32;
        if (diff < 0.0f) {
            diff = -diff;
        }
        return diff < 1e-3f;
    }
    default:
        return a->kind == HA_VALUE_NONE;
    }
}

/* Проекция raw-записи (то, что видит legacy-читатель) должна совпасть с canonical-записью. */
static void assert_parity(ctx_t *c, ha_device_uid_t uid, uint8_t ep, uint16_t cluster,
                          uint16_t attr)
{
    ha_zb_state_key_t raw_key = {
        .device_uid = uid,
        .cluster_id = cluster,
        .attr_id = attr,
        .endpoint = ep,
    };
    ha_zb_state_record_t raw_record = {0};
    CHECK(sys_ok(domain_entity_get(&c->d, (domain_entity_t)HA_ENTITY_ZB_DIAG_STATE, &raw_key, &raw_record)));

    ha_value_t projected = {0};
    CHECK(semantics_state_value(&raw_key, &raw_record, &projected));

    ha_entity_id_t entity_id = HA_ENTITY_ID_NONE;
    CHECK(zb_entity_binding_find_entity(c->reg, c->n, uid, ep, &entity_id));

    const ha_state_key_t key = {
        .entity = entity_id,
        .property = semantics_property_from_key(&raw_key),
    };
    ha_state_record_t canonical = {0};
    CHECK(sys_ok(domain_entity_get(&c->d, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &key,
                                   &canonical)));
    CHECK(value_equal(&projected, &canonical.value));
}

static bool count_cb(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    (*(size_t *)ctx)++;
    return true;
}

static size_t count_type(ctx_t *c, domain_entity_t type)
{
    size_t n = 0;
    CHECK(sys_ok(domain_entity_iter(&c->d, type, count_cb, &n)));
    return n;
}

static void test_known_properties_match(void)
{
    ctx_t c;
    ctx_init(&c);
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;
    CHECK(sys_ok(zigbee_device_ensure(&c.d, uid)));

    feed(&c, uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_ZB_TYPE_BOOL, 1);
    feed(&c, uid, 1, HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, HA_ZB_TYPE_UINT8,
         127);
    feed(&c, uid, 1, HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
         HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE, HA_ZB_TYPE_INT16, 2150);

    assert_parity(&c, uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF);
    assert_parity(&c, uid, 1, HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL);
    assert_parity(&c, uid, 1, HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
                  HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE);

    /* canonical несёт смысл, а не raw: 127 → ~50 %, 2150 → 21.50 °C. */
    const ha_state_key_t bright_key = {
        .entity = c.reg[0].entity_id,
        .property = HA_PROPERTY_BRIGHTNESS,
    };
    ha_state_record_t bright = {0};
    CHECK(sys_ok(domain_entity_get(&c.d, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &bright_key,
                                   &bright)));
    CHECK(bright.value.kind == HA_VALUE_FLOAT);
    CHECK(bright.value.value.f32 > 49.0f && bright.value.value.f32 < 51.0f);

    CHECK(count_type(&c, (domain_entity_t)HA_ENTITY_PROPERTY_STATE) == 3);

    domain_deinit(&c.d);
}

/* Атрибут вне словаря: raw остаётся (truth), canonical не выдумывается. */
static void test_unmapped_attr_has_no_canonical(void)
{
    ctx_t c;
    ctx_init(&c);
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;
    CHECK(sys_ok(zigbee_device_ensure(&c.d, uid)));

    feed(&c, uid, 1, 0xFFF0u, 0x0001u, HA_ZB_TYPE_UINT8, 42);

    ha_zb_state_key_t raw_key = {
        .device_uid = uid,
        .cluster_id = 0xFFF0u,
        .attr_id = 0x0001u,
        .endpoint = 1,
    };
    ha_zb_state_record_t raw_record = {0};
    CHECK(sys_ok(domain_entity_get(&c.d, (domain_entity_t)HA_ENTITY_ZB_DIAG_STATE, &raw_key, &raw_record)));
    CHECK(count_type(&c, (domain_entity_t)HA_ENTITY_PROPERTY_STATE) == 0);

    domain_deinit(&c.d);
}

/* Один атрибут на разных endpoint — разный entity_id, значит разные canonical-записи. */
static void test_endpoint_is_part_of_address(void)
{
    ctx_t c;
    ctx_init(&c);
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;
    CHECK(sys_ok(zigbee_device_ensure(&c.d, uid)));

    feed(&c, uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_ZB_TYPE_BOOL, 1);
    feed(&c, uid, 2, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_ZB_TYPE_BOOL, 0);

    CHECK(c.n == 2);
    CHECK(c.reg[0].entity_id != c.reg[1].entity_id);
    CHECK(count_type(&c, (domain_entity_t)HA_ENTITY_PROPERTY_STATE) == 2);
    assert_parity(&c, uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF);
    assert_parity(&c, uid, 2, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF);

    domain_deinit(&c.d);
}

int main(void)
{
    test_known_properties_match();
    test_unmapped_attr_has_no_canonical();
    test_endpoint_is_part_of_address();

    if (g_failures == 0) {
        printf("all property-state tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
