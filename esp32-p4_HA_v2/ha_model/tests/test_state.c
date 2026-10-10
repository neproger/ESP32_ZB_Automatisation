#include "ha_model/ha_state.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * A1.0 (docs/STEP_A_PLAN.md): canonical state key/record — transport-agnostic адрес
 * (entity_id, property_id) и семантическое значение. Здесь только форма: связь с
 * Zigbee/raw-таблицей появляется в A1.1.
 */

_Static_assert(sizeof(ha_state_key_t) == 16, "state key ABI");
_Static_assert(offsetof(ha_state_key_t, entity) == 0, "state key layout");
_Static_assert(offsetof(ha_state_key_t, property) == 8, "state key layout");
_Static_assert(sizeof(ha_state_record_t) == 8, "state record ABI");
_Static_assert(HA_PROPERTY_POWER == 1, "property ABI");

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_key_addresses_entity_and_property(void)
{
    const ha_state_key_t key = {
        .entity = 0xdf803cbaf588876dull,
        .property = HA_PROPERTY_TEMPERATURE,
    };
    CHECK(key.entity == 0xdf803cbaf588876dull);
    CHECK(key.property == HA_PROPERTY_TEMPERATURE);
}

static void test_zeroed_record_is_no_value(void)
{
    ha_state_record_t rec;
    memset(&rec, 0, sizeof(rec)); /* HA_VALUE_NONE — «значения нет» */
    CHECK(rec.value.kind == HA_VALUE_NONE);
}

static void test_record_carries_semantic_value(void)
{
    ha_state_record_t rec = {0};
    rec.value.kind = HA_VALUE_FLOAT;
    rec.value.value.f32 = 21.5f;
    CHECK(rec.value.kind == HA_VALUE_FLOAT);
    CHECK(rec.value.value.f32 == 21.5f);
}

/* Адрес различает и сущность, и свойство: ключи несовместимы, если отличается любое поле. */
static void test_key_fields_are_distinct(void)
{
    const ha_state_key_t power = {.entity = 1, .property = HA_PROPERTY_POWER};
    const ha_state_key_t brightness = {.entity = 1, .property = HA_PROPERTY_BRIGHTNESS};
    const ha_state_key_t other_entity = {.entity = 2, .property = HA_PROPERTY_POWER};

    CHECK(power.property != brightness.property); /* то же entity, иное свойство */
    CHECK(power.entity != other_entity.entity);   /* иное entity, то же свойство */
}

int main(void)
{
    test_key_addresses_entity_and_property();
    test_zeroed_record_is_no_value();
    test_record_carries_semantic_value();
    test_key_fields_are_distinct();

    if (g_failures == 0) {
        printf("all state tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
