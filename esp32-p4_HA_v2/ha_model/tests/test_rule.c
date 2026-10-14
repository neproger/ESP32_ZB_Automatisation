#include "ha_model/ha_rule.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * A3.0 + списки (docs/STEP_A_PLAN.md, docs/services/AUTOMATION.md): хранимая
 * entity-адресованная форма правила. Проверяем layout (детерминированный POD), лимиты,
 * сентинел SOURCE, поля триггеров/действий.
 */

_Static_assert(sizeof(ha_rule_t) == 392, "rule storage ABI");
_Static_assert(offsetof(ha_rule_t, triggers) == 0, "rule layout triggers");
_Static_assert(offsetof(ha_rule_t, actions) == 128, "rule layout actions");
_Static_assert(offsetof(ha_rule_t, conditions) == 256, "rule layout conditions");
_Static_assert(sizeof(ha_rule_trigger_t) == 32, "trigger ABI");
_Static_assert(sizeof(ha_rule_action_t) == 32, "action ABI");
_Static_assert(HA_RULE_TRIGGERS_MAX == 4 && HA_RULE_ACTIONS_MAX == 4, "rule limits");
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

static void test_triggers_conditions_actions(void)
{
    ha_rule_t rule = {0};
    rule.enabled = 1;

    rule.trigger_count = 2;
    rule.triggers[0].kind = HA_TRIGGER_STATE;
    rule.triggers[0].entity = HA_ENTITY_ID_SOURCE;
    rule.triggers[0].property = HA_PROPERTY_POWER;
    rule.triggers[0].op = HA_CONDITION_OP_EQ;
    rule.triggers[0].edge = HA_TRIGGER_EDGE_RISING;
    rule.triggers[0].value = 1.0f;
    rule.triggers[1].kind = HA_TRIGGER_TIME;
    rule.triggers[1].minutes_of_day = 7 * 60 + 30;
    rule.triggers[1].weekday_mask = 0x1Fu;

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

    rule.action_count = 2;
    rule.actions[0].ref.entity = 0x1122334455667788ull;
    rule.actions[0].ref.property = HA_PROPERTY_BRIGHTNESS;
    rule.actions[0].action = HA_ACTION_SET;
    rule.actions[0].value.kind = HA_COMMAND_VALUE_SCALAR;
    rule.actions[0].value.value.scalar.kind = HA_VALUE_U32;
    rule.actions[0].value.value.scalar.value.u32 = 75;
    rule.actions[1].ref.entity = HA_ENTITY_ID_SOURCE;
    rule.actions[1].ref.property = HA_PROPERTY_POWER;
    rule.actions[1].action = HA_ACTION_ON;

    CHECK(rule.trigger_count == 2);
    CHECK(rule.triggers[0].kind == HA_TRIGGER_STATE);
    CHECK(rule.triggers[0].entity == HA_ENTITY_ID_SOURCE);
    CHECK(rule.triggers[1].minutes_of_day == 450);
    CHECK(rule.conditions_count == 2);
    CHECK(rule.conditions[0].cmp.value2 == 24.0f);
    CHECK(rule.conditions[1].ref.entity == HA_ENTITY_ID_SOURCE);
    CHECK(rule.action_count == 2);
    CHECK(rule.actions[0].ref.property == HA_PROPERTY_BRIGHTNESS);
    CHECK(rule.actions[0].value.value.scalar.value.u32 == 75);
    CHECK(rule.actions[1].action == HA_ACTION_ON);
}

static void test_event_trigger_carries_entity(void)
{
    ha_rule_t rule = {0};
    rule.trigger_count = 1;
    rule.triggers[0].kind = HA_TRIGGER_DEVICE_EVENT;
    rule.triggers[0].entity = 0x0011223344556677ull;
    rule.triggers[0].event_id = HA_EVENT_SINGLE_PRESS;
    rule.action_count = 1;
    rule.actions[0].ref.entity = HA_ENTITY_ID_SOURCE;
    rule.actions[0].ref.property = HA_PROPERTY_POWER;
    rule.actions[0].action = HA_ACTION_TOGGLE;
    rule.actions[0].value.kind = HA_COMMAND_VALUE_NONE;

    CHECK(rule.triggers[0].entity == 0x0011223344556677ull);
    CHECK(rule.triggers[0].event_id == HA_EVENT_SINGLE_PRESS);
    CHECK(rule.actions[0].action == HA_ACTION_TOGGLE);
    CHECK(rule.actions[0].value.kind == HA_COMMAND_VALUE_NONE);
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
    test_triggers_conditions_actions();
    test_event_trigger_carries_entity();
    test_zeroed_record_is_all_zero();

    if (g_failures == 0) {
        printf("all rule tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
