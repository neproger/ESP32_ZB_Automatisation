#include "ha_model/ha_rule.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * A3.0 (docs/STEP_A_PLAN.md): хранимая entity-адресованная форма правила. Проверяем
 * форму: детерминированный layout, сентинел SOURCE, поля по видам триггера, action.
 */

_Static_assert(sizeof(ha_rule_t) == 192, "rule storage ABI");
_Static_assert(offsetof(ha_rule_t, action_ref) == 0, "rule layout action_ref");
_Static_assert(offsetof(ha_rule_t, action_value) == 176, "rule layout action_value");
_Static_assert(HA_ENTITY_ID_SOURCE != HA_ENTITY_ID_NONE, "source != none");

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_source_is_distinct_sentinel(void)
{
    CHECK(HA_ENTITY_ID_SOURCE != HA_ENTITY_ID_NONE);
    CHECK(HA_ENTITY_ID_SOURCE != 0xdf803cbaf588876dull); /* не совпадает с выведенным id */
}

static void test_state_trigger_and_conditions(void)
{
    ha_rule_t rule = {0};
    rule.enabled = 1;
    rule.trigger.kind = HA_TRIGGER_STATE;
    rule.trigger.entity = HA_ENTITY_ID_SOURCE;
    rule.trigger.property = HA_PROPERTY_POWER;
    rule.trigger.op = HA_CONDITION_OP_EQ;
    rule.trigger.edge = HA_TRIGGER_EDGE_RISING;
    rule.trigger.value = 1.0f;

    rule.conditions_count = 2;
    rule.conditions[0].ref.entity = 0x1122334455667788ull;
    rule.conditions[0].ref.property = HA_PROPERTY_TEMPERATURE;
    rule.conditions[0].cmp.op = HA_CONDITION_OP_BETWEEN;
    rule.conditions[0].cmp.value = 18.0f;
    rule.conditions[0].cmp.value2 = 24.0f;
    rule.conditions[1].ref.entity = HA_ENTITY_ID_SOURCE;
    rule.conditions[1].ref.property = HA_PROPERTY_SYSTEM_HOUR;
    rule.conditions[1].cmp.op = HA_CONDITION_OP_GE;
    rule.conditions[1].cmp.value = 7.0f;

    rule.action_ref.entity = 0x1122334455667788ull;
    rule.action_ref.property = HA_PROPERTY_BRIGHTNESS;
    rule.action = HA_ACTION_SET;
    rule.action_value.kind = HA_COMMAND_VALUE_SCALAR;
    rule.action_value.value.scalar.kind = HA_VALUE_U32;
    rule.action_value.value.scalar.value.u32 = 75;

    CHECK(rule.trigger.kind == HA_TRIGGER_STATE);
    CHECK(rule.trigger.entity == HA_ENTITY_ID_SOURCE);
    CHECK(rule.conditions_count == 2);
    CHECK(rule.conditions[0].cmp.value2 == 24.0f);
    CHECK(rule.conditions[1].ref.entity == HA_ENTITY_ID_SOURCE);
    CHECK(rule.action_ref.property == HA_PROPERTY_BRIGHTNESS);
    CHECK(rule.action_value.value.scalar.value.u32 == 75);
}

static void test_event_trigger_carries_entity(void)
{
    ha_rule_t rule = {0};
    rule.trigger.kind = HA_TRIGGER_DEVICE_EVENT;
    rule.trigger.entity = 0x0011223344556677ull;
    rule.trigger.event_id = HA_EVENT_SINGLE_PRESS;
    rule.action_ref.entity = HA_ENTITY_ID_SOURCE;
    rule.action_ref.property = HA_PROPERTY_POWER;
    rule.action = HA_ACTION_TOGGLE;
    rule.action_value.kind = HA_COMMAND_VALUE_NONE;

    CHECK(rule.trigger.entity == 0x0011223344556677ull);
    CHECK(rule.trigger.event_id == HA_EVENT_SINGLE_PRESS);
    CHECK(rule.action == HA_ACTION_TOGGLE);
    CHECK(rule.action_value.kind == HA_COMMAND_VALUE_NONE);
}

static void test_time_trigger(void)
{
    ha_rule_t rule = {0};
    rule.trigger.kind = HA_TRIGGER_TIME;
    rule.trigger.minutes_of_day = 7 * 60 + 30;
    rule.trigger.weekday_mask = 0x1Fu; /* Пн..Пт */

    CHECK(rule.trigger.minutes_of_day == 450);
    CHECK((rule.trigger.weekday_mask & 0x1Fu) == 0x1Fu);
}

/* Байтовое представление детерминировано: {0} → нули, отдельные поля не «шумят» хвост. */
static void test_zeroed_record_is_all_zero(void)
{
    ha_rule_t rule = {0};
    const uint8_t *bytes = (const uint8_t *)&rule;
    for (size_t i = 0; i < sizeof(rule); i++) {
        CHECK(bytes[i] == 0);
    }
}

int main(void)
{
    test_source_is_distinct_sentinel();
    test_state_trigger_and_conditions();
    test_event_trigger_carries_entity();
    test_time_trigger();
    test_zeroed_record_is_all_zero();

    if (g_failures == 0) {
        printf("all rule tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
