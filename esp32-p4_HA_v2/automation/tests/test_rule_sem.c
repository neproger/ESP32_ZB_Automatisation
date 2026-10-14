#include "automation/automation_rule_sem.h"

#include <stdio.h>

/*
 * A3.3 (docs/STEP_A_PLAN.md): чистый semantic-evaluator. Проверяем матчинг одного
 * триггера (EVENT/STATE/TIME), операторы, edge, резолв SOURCE и сборку semantic-команды
 * одного действия. Никакого uid/endpoint/cluster/attr здесь нет. `enabled` правила
 * проверяет engine, а не эти функции — поэтому здесь его нет.
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
    ha_rule_trigger_t t = {0};
    t.kind = HA_TRIGGER_DEVICE_EVENT;
    t.entity = E;
    t.event_id = HA_EVENT_SINGLE_PRESS;

    CHECK(automation_rule_sem_event_matches(&t, E, HA_EVENT_SINGLE_PRESS));
    CHECK(!automation_rule_sem_event_matches(&t, E, HA_EVENT_DOUBLE_PRESS));
    CHECK(!automation_rule_sem_event_matches(&t, F, HA_EVENT_SINGLE_PRESS));

    /* SOURCE: событие от источника-триггера. */
    t.entity = HA_ENTITY_ID_SOURCE;
    CHECK(automation_rule_sem_event_matches(&t, F, HA_EVENT_SINGLE_PRESS));
    /* источник неизвестен → SOURCE не резолвится, матча нет */
    CHECK(!automation_rule_sem_event_matches(&t, HA_ENTITY_ID_NONE, HA_EVENT_SINGLE_PRESS));

    /* не тот kind / NULL */
    t.kind = HA_TRIGGER_TIME;
    CHECK(!automation_rule_sem_event_matches(&t, E, HA_EVENT_SINGLE_PRESS));
    CHECK(!automation_rule_sem_event_matches(NULL, E, HA_EVENT_SINGLE_PRESS));
}

static void test_state_and_edge(void)
{
    ha_rule_trigger_t t = {0};
    t.kind = HA_TRIGGER_STATE;
    t.entity = E;
    t.property = HA_PROPERTY_POWER;
    t.op = HA_CONDITION_OP_EQ;
    t.edge = HA_TRIGGER_EDGE_RISING;
    t.value = 1.0f;

    /* стало истинно: было 0 → 1 */
    CHECK(automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_POWER, 1.0, true, 0.0));
    /* первое наблюдение истинно → rising */
    CHECK(automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_POWER, 1.0, false, 0.0));
    /* всё ещё истинно (prev тоже истинно) → не rising */
    CHECK(!automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_POWER, 1.0, true, 1.0));
    /* значение ложно */
    CHECK(!automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_POWER, 0.0, true, 1.0));
    /* другое свойство/entity */
    CHECK(!automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_BRIGHTNESS, 1.0, true, 0.0));
    CHECK(!automation_rule_sem_state_matches(&t, E, F, HA_PROPERTY_POWER, 1.0, true, 0.0));

    /* SOURCE резолвится в источник */
    t.entity = HA_ENTITY_ID_SOURCE;
    CHECK(automation_rule_sem_state_matches(&t, F, F, HA_PROPERTY_POWER, 1.0, true, 0.0));
    /* у изменения нет адреса → ветка не наша */
    CHECK(!automation_rule_sem_state_matches(&t, F, HA_ENTITY_ID_NONE, HA_PROPERTY_POWER, 1.0, true,
                                             0.0));

    /* FALLING */
    t.entity = E;
    t.edge = HA_TRIGGER_EDGE_FALLING;
    CHECK(automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_POWER, 0.0, true, 1.0));

    /* BETWEEN: 18..24 */
    t.op = HA_CONDITION_OP_BETWEEN;
    t.edge = HA_TRIGGER_EDGE_ANY;
    t.value = 18.0f;
    t.value2 = 24.0f;
    t.property = HA_PROPERTY_TEMPERATURE;
    CHECK(automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_TEMPERATURE, 21.0, true, 0.0));
    CHECK(!automation_rule_sem_state_matches(&t, E, E, HA_PROPERTY_TEMPERATURE, 25.0, true, 0.0));
}

static void test_time(void)
{
    ha_rule_trigger_t t = {0};
    t.kind = HA_TRIGGER_TIME;
    t.minutes_of_day = 7 * 60 + 30;
    t.weekday_mask = 0x1Fu; /* Пн..Пт */

    CHECK(automation_rule_sem_time_matches(&t, 450, 0x01u));  /* Пн */
    CHECK(!automation_rule_sem_time_matches(&t, 451, 0x01u)); /* другая минута */
    CHECK(!automation_rule_sem_time_matches(&t, 450, 0x20u)); /* Сб не в маске */
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
    ha_rule_action_t a = {0};
    a.ref.entity = E;
    a.ref.property = HA_PROPERTY_BRIGHTNESS;
    a.action = HA_ACTION_SET;
    a.value.kind = HA_COMMAND_VALUE_SCALAR;
    a.value.value.scalar.kind = HA_VALUE_U32;
    a.value.value.scalar.value.u32 = 75;

    ha_property_command_t command = {0};
    CHECK(automation_rule_sem_action(&a, E, &command));
    CHECK(command.entity == E);
    CHECK(command.property == HA_PROPERTY_BRIGHTNESS);
    CHECK(command.action == HA_ACTION_SET);
    CHECK(command.value.kind == HA_COMMAND_VALUE_SCALAR);
    CHECK(command.value.value.scalar.value.u32 == 75);

    /* SOURCE → источник */
    a.ref.entity = HA_ENTITY_ID_SOURCE;
    CHECK(automation_rule_sem_action(&a, F, &command));
    CHECK(command.entity == F);

    /* SOURCE без источника (source=NONE) → невалидно */
    CHECK(!automation_rule_sem_action(&a, HA_ENTITY_ID_NONE, &command));

    /* неизвестное действие → невалидно */
    a.ref.entity = E;
    a.action = HA_ACTION_NONE;
    CHECK(!automation_rule_sem_action(&a, E, &command));
}

/* A3.5: поведенческий parity — типовые правила на entity-модели. */
static void test_behaviour_parity(void)
{
    ha_property_command_t cmd = {0};
    const ha_entity_id_t SYSTEM = 0xABCDEF0123456789ull;

    ha_rule_trigger_t trig = {0};
    ha_rule_action_t act = {0};

    /* TIME -> POWER ON (источник TIME-тика = ENTITY_SYSTEM). */
    trig.kind = HA_TRIGGER_TIME;
    trig.minutes_of_day = 7 * 60 + 30;
    trig.weekday_mask = 0x7f;
    act.ref.entity = E;
    act.ref.property = HA_PROPERTY_POWER;
    act.action = HA_ACTION_ON;
    CHECK(automation_rule_sem_time_matches(&trig, 450, 0x01));
    CHECK(automation_rule_sem_action(&act, SYSTEM, &cmd));
    CHECK(cmd.entity == E && cmd.property == HA_PROPERTY_POWER && cmd.action == HA_ACTION_ON);

    /* TEMP > 25 (STATE-триггер). */
    trig.kind = HA_TRIGGER_STATE;
    trig.entity = E;
    trig.property = HA_PROPERTY_TEMPERATURE;
    trig.op = HA_CONDITION_OP_GT;
    trig.edge = HA_TRIGGER_EDGE_ANY;
    trig.value = 25.0f;
    CHECK(automation_rule_sem_state_matches(&trig, E, E, HA_PROPERTY_TEMPERATURE, 26.0, true, 24.0));
    CHECK(!automation_rule_sem_state_matches(&trig, E, E, HA_PROPERTY_TEMPERATURE, 24.0, true, 26.0));

    /* EVENT SINGLE_PRESS -> TOGGLE (действие — на источник события). */
    trig.kind = HA_TRIGGER_DEVICE_EVENT;
    trig.entity = HA_ENTITY_ID_SOURCE;
    trig.event_id = HA_EVENT_SINGLE_PRESS;
    act.ref.entity = HA_ENTITY_ID_SOURCE;
    act.ref.property = HA_PROPERTY_POWER;
    act.action = HA_ACTION_TOGGLE;
    CHECK(automation_rule_sem_event_matches(&trig, F, HA_EVENT_SINGLE_PRESS));
    CHECK(automation_rule_sem_action(&act, F, &cmd));
    CHECK(cmd.entity == F && cmd.action == HA_ACTION_TOGGLE);

    /* Условие POWER=true. */
    ha_rule_condition_t cond = {0};
    cond.ref.entity = E;
    cond.ref.property = HA_PROPERTY_POWER;
    cond.cmp.op = HA_CONDITION_OP_EQ;
    cond.cmp.value = 1.0f;
    CHECK(automation_rule_sem_condition_ok(&cond, 1.0));
    CHECK(!automation_rule_sem_condition_ok(&cond, 0.0));

    /* Действие BRIGHTNESS 70. */
    act.ref.entity = E;
    act.ref.property = HA_PROPERTY_BRIGHTNESS;
    act.action = HA_ACTION_SET;
    act.value.kind = HA_COMMAND_VALUE_SCALAR;
    act.value.value.scalar.kind = HA_VALUE_U32;
    act.value.value.scalar.value.u32 = 70;
    CHECK(automation_rule_sem_action(&act, F, &cmd));
    CHECK(cmd.entity == E && cmd.property == HA_PROPERTY_BRIGHTNESS);
    CHECK(cmd.value.value.scalar.value.u32 == 70);
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
    test_behaviour_parity();

    if (g_failures == 0) {
        printf("all semantic-rule tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
