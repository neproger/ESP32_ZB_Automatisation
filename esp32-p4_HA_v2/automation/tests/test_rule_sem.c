#include "automation/automation_rule_sem.h"

#include <stdio.h>

/*
 * A3.3 (docs/STEP_A_PLAN.md): чистый semantic-evaluator правила ha_rule_t. Проверяем
 * матчинг EVENT/STATE/TIME, операторы, edge, резолв SOURCE и сборку semantic-команды.
 * Никакого uid/endpoint/cluster/attr здесь нет.
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define E 0x0011223344556677ull
#define F 0x8899AABBCCDDEEFFull

static void test_resolve(void)
{
    CHECK(automation_rule_sem_resolve(HA_ENTITY_ID_SOURCE, E) == E);
    CHECK(automation_rule_sem_resolve(F, E) == F);
    CHECK(automation_rule_sem_resolve(HA_ENTITY_ID_NONE, E) == HA_ENTITY_ID_NONE);
}

static void test_event(void)
{
    ha_rule_t rule = {0};
    rule.enabled = 1;
    rule.trigger.kind = HA_TRIGGER_DEVICE_EVENT;
    rule.trigger.entity = E;
    rule.trigger.event_id = HA_EVENT_SINGLE_PRESS;

    CHECK(automation_rule_sem_event_matches(&rule, E, HA_EVENT_SINGLE_PRESS));
    CHECK(!automation_rule_sem_event_matches(&rule, E, HA_EVENT_DOUBLE_PRESS));
    CHECK(!automation_rule_sem_event_matches(&rule, F, HA_EVENT_SINGLE_PRESS));

    /* SOURCE: любое событие этого id от источника-триггера. */
    rule.trigger.entity = HA_ENTITY_ID_SOURCE;
    CHECK(automation_rule_sem_event_matches(&rule, F, HA_EVENT_SINGLE_PRESS));

    rule.enabled = 0;
    CHECK(!automation_rule_sem_event_matches(&rule, F, HA_EVENT_SINGLE_PRESS));
}

static void test_state_and_edge(void)
{
    ha_rule_t rule = {0};
    rule.enabled = 1;
    rule.trigger.kind = HA_TRIGGER_STATE;
    rule.trigger.entity = E;
    rule.trigger.property = HA_PROPERTY_POWER;
    rule.trigger.op = HA_CONDITION_OP_EQ;
    rule.trigger.edge = HA_TRIGGER_EDGE_RISING;
    rule.trigger.value = 1.0f;

    /* стало истинно: было 0 → 1 */
    CHECK(automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_POWER, 1.0, true, 0.0));
    /* первое наблюдение истинно → rising */
    CHECK(automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_POWER, 1.0, false, 0.0));
    /* всё ещё истинно (prev тоже истинно) → не rising */
    CHECK(!automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_POWER, 1.0, true, 1.0));
    /* значение ложно */
    CHECK(!automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_POWER, 0.0, true, 1.0));
    /* другое свойство/entity */
    CHECK(!automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_BRIGHTNESS, 1.0, true, 0.0));
    CHECK(!automation_rule_sem_state_matches(&rule, E, F, HA_PROPERTY_POWER, 1.0, true, 0.0));

    /* SOURCE резолвится в источник */
    rule.trigger.entity = HA_ENTITY_ID_SOURCE;
    CHECK(automation_rule_sem_state_matches(&rule, F, F, HA_PROPERTY_POWER, 1.0, true, 0.0));

    /* FALLING */
    rule.trigger.entity = E;
    rule.trigger.edge = HA_TRIGGER_EDGE_FALLING;
    CHECK(automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_POWER, 0.0, true, 1.0));

    /* BETWEEN: 18..24 */
    rule.trigger.op = HA_CONDITION_OP_BETWEEN;
    rule.trigger.edge = HA_TRIGGER_EDGE_ANY;
    rule.trigger.value = 18.0f;
    rule.trigger.value2 = 24.0f;
    rule.trigger.property = HA_PROPERTY_TEMPERATURE;
    CHECK(automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_TEMPERATURE, 21.0, true, 0.0));
    CHECK(!automation_rule_sem_state_matches(&rule, E, E, HA_PROPERTY_TEMPERATURE, 25.0, true, 0.0));
}

static void test_time(void)
{
    ha_rule_t rule = {0};
    rule.enabled = 1;
    rule.trigger.kind = HA_TRIGGER_TIME;
    rule.trigger.minutes_of_day = 7 * 60 + 30;
    rule.trigger.weekday_mask = 0x1Fu; /* Пн..Пт */

    CHECK(automation_rule_sem_time_matches(&rule, 450, 0x01u));   /* Пн */
    CHECK(!automation_rule_sem_time_matches(&rule, 451, 0x01u));  /* другая минута */
    CHECK(!automation_rule_sem_time_matches(&rule, 450, 0x20u));  /* Сб не в маске */
}

static void test_compare(void)
{
    CHECK(automation_rule_sem_compare(HA_CONDITION_OP_EQ, 5.0, 0.0, 5.0));
    CHECK(!automation_rule_sem_compare(HA_CONDITION_OP_EQ, 5.0, 0.0, 5.1));
    CHECK(automation_rule_sem_compare(HA_CONDITION_OP_GE, 5.0, 0.0, 5.0));
    CHECK(automation_rule_sem_compare(HA_CONDITION_OP_BETWEEN, 1.0, 3.0, 2.0));
    CHECK(!automation_rule_sem_compare(HA_CONDITION_OP_BETWEEN, 1.0, 3.0, 4.0));
    CHECK(automation_rule_sem_compare(HA_CONDITION_OP_HAS_BITS, 0x04, 0.0, 0x05)); /* бит 2 */
    CHECK(!automation_rule_sem_compare(HA_CONDITION_OP_HAS_BITS, 0x04, 0.0, 0x01));
}

static void test_condition(void)
{
    ha_rule_condition_t cond = {0};
    cond.cmp.op = HA_CONDITION_OP_GT;
    cond.cmp.value = 10.0f;
    CHECK(automation_rule_sem_condition_ok(&cond, 11.0));
    CHECK(!automation_rule_sem_condition_ok(&cond, 9.0));
    CHECK(!automation_rule_sem_condition_ok(NULL, 11.0));
}

static void test_action(void)
{
    ha_rule_t rule = {0};
    rule.enabled = 1;
    rule.action_ref.entity = E;
    rule.action_ref.property = HA_PROPERTY_BRIGHTNESS;
    rule.action = HA_ACTION_SET;
    rule.action_value.kind = HA_COMMAND_VALUE_SCALAR;
    rule.action_value.value.scalar.kind = HA_VALUE_U32;
    rule.action_value.value.scalar.value.u32 = 75;

    ha_property_command_t command = {0};
    CHECK(automation_rule_sem_action(&rule, E, &command));
    CHECK(command.entity == E);
    CHECK(command.property == HA_PROPERTY_BRIGHTNESS);
    CHECK(command.action == HA_ACTION_SET);
    CHECK(command.value.kind == HA_COMMAND_VALUE_SCALAR);
    CHECK(command.value.value.scalar.value.u32 == 75);

    /* SOURCE → источник */
    rule.action_ref.entity = HA_ENTITY_ID_SOURCE;
    CHECK(automation_rule_sem_action(&rule, F, &command));
    CHECK(command.entity == F);

    /* SOURCE без источника (source=NONE) → невалидно */
    CHECK(!automation_rule_sem_action(&rule, HA_ENTITY_ID_NONE, &command));

    /* неизвестное действие → невалидно */
    rule.action_ref.entity = E;
    rule.action = HA_ACTION_NONE;
    CHECK(!automation_rule_sem_action(&rule, E, &command));
}

int main(void)
{
    test_resolve();
    test_event();
    test_state_and_edge();
    test_time();
    test_compare();
    test_condition();
    test_action();

    if (g_failures == 0) {
        printf("all semantic-rule tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
