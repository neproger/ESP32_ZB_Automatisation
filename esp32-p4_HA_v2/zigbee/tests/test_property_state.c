#include "domain/domain.h"
#include "semantics/semantics.h"
#include "zigbee/zigbee_entity_binding.h"
#include "zigbee/zigbee_state.h"

#include <stdio.h>
#include <string.h>

/*
 * A1.3/A3.3: проекция репорта в canonical property state. Доказываем, что смысл берётся
 * у моста (cluster/attr → property, raw+type → semantic value), а не выдумывается:
 *   raw:       (uid, ep, cluster, attr) -> semantics -> value
 *   canonical: (entity_id, property_id)            -> value
 */

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

    const domain_entity_desc_t canonical = {
        .type = (domain_entity_t)HA_ENTITY_PROPERTY_STATE,
        .key_size = sizeof(ha_state_key_t),
        .payload_size = sizeof(ha_state_record_t),
        .capacity = 16,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    domain_register_entity(&c->d, &canonical);
}

/* Прогон одного репорта ровно как в сервисе: canonical пишется, только если endpoint уже
 * имеет Entity и значение имеет смысл. */
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

    feed(&c, uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_ZB_TYPE_BOOL, 1);
    feed(&c, uid, 1, HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, HA_ZB_TYPE_UINT8,
         127);
    feed(&c, uid, 1, HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
         HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE, HA_ZB_TYPE_INT16, 2150);

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

    const ha_state_key_t temp_key = {
        .entity = c.reg[0].entity_id,
        .property = HA_PROPERTY_TEMPERATURE,
    };
    ha_state_record_t temp = {0};
    CHECK(sys_ok(domain_entity_get(&c.d, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &temp_key,
                                   &temp)));
    CHECK(temp.value.kind == HA_VALUE_FLOAT);
    CHECK(temp.value.value.f32 > 21.4f && temp.value.value.f32 < 21.6f);

    CHECK(count_type(&c, (domain_entity_t)HA_ENTITY_PROPERTY_STATE) == 3);

    domain_deinit(&c.d);
}

/* Атрибут вне словаря: canonical не выдумывается. */
static void test_unmapped_attr_has_no_canonical(void)
{
    ctx_t c;
    ctx_init(&c);
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;

    feed(&c, uid, 1, 0xFFF0u, 0x0001u, HA_ZB_TYPE_UINT8, 42);

    CHECK(count_type(&c, (domain_entity_t)HA_ENTITY_PROPERTY_STATE) == 0);

    domain_deinit(&c.d);
}

/* Один атрибут на разных endpoint — разный entity_id, значит разные canonical-записи. */
static void test_endpoint_is_part_of_address(void)
{
    ctx_t c;
    ctx_init(&c);
    const ha_device_uid_t uid = 0x00124B000A1B2C3Dull;

    feed(&c, uid, 1, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_ZB_TYPE_BOOL, 1);
    feed(&c, uid, 2, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_ZB_TYPE_BOOL, 0);

    CHECK(c.n == 2);
    CHECK(c.reg[0].entity_id != c.reg[1].entity_id);
    CHECK(count_type(&c, (domain_entity_t)HA_ENTITY_PROPERTY_STATE) == 2);

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
