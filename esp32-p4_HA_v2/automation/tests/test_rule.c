#include "automation/automation_rule.h"

#include <stdio.h>
#include <string.h>

#include "ha_model/ha_commands.h"
#include "ha_model/ha_zigbee.h"

/*
 * Правило: сопоставление с событием (device, command) и сборка Zigbee-команды
 * (docs/services/AUTOMATION.md).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static const ha_device_uid_t UID = 0x00124B000A1B2C3Dull;

static ha_automation_record_t demo_rule(void)
{
    ha_automation_record_t rule = {0};
    rule.enabled = 1;
    rule.trigger_b.event.device_uid = UID;
    rule.trigger_b.event.command_id = HA_ZB_CMD_ON_OFF_TOGGLE;
    rule.action_endpoint = 2;
    rule.action_cluster_id = HA_ZB_CLUSTER_ON_OFF;
    rule.action_command_id = HA_ZB_CMD_ON_OFF_TOGGLE;
    return rule;
}

static void test_matches(void)
{
    ha_automation_record_t rule = demo_rule();
    CHECK(automation_rule_matches(&rule, UID, HA_ZB_CMD_ON_OFF_TOGGLE));
    CHECK(!automation_rule_matches(&rule, UID, HA_ZB_CMD_ON_OFF_ON));
    CHECK(!automation_rule_matches(&rule, UID + 1, HA_ZB_CMD_ON_OFF_TOGGLE));

    rule.enabled = 0;
    CHECK(!automation_rule_matches(&rule, UID, HA_ZB_CMD_ON_OFF_TOGGLE));

    ha_automation_record_t any = {0};
    any.enabled = 1; /* триггер «любое устройство, любая команда» */
    CHECK(automation_rule_matches(&any, UID, 0x42));
}

static void test_command(void)
{
    const ha_automation_record_t rule = demo_rule();
    ha_zb_command_t command = {0};
    CHECK(sys_ok(automation_rule_command(&rule, UID, &command)));
    CHECK(command.device_uid == UID); /* action_device_uid == 0 → устройство-источник */
    CHECK(command.dst_endpoint == 2);
    CHECK(command.cluster_id == HA_ZB_CLUSTER_ON_OFF);
    CHECK(command.command_id == HA_ZB_CMD_ON_OFF_TOGGLE);
    CHECK(command.args_len == 0);

    ha_automation_record_t other = demo_rule();
    other.action_device_uid = UID + 1;
    CHECK(sys_ok(automation_rule_command(&other, UID, &command)));
    CHECK(command.device_uid == UID + 1);

    ha_automation_record_t level = demo_rule();
    level.action_args_len = 1;
    level.action_args[0] = 200;
    CHECK(sys_ok(automation_rule_command(&level, UID, &command)));
    CHECK(command.args_len == 1);
    CHECK(command.args[0] == 200);

    level.action_args_len = HA_ZB_COMMAND_ARGS_MAX + 1;
    CHECK(sys_is(automation_rule_command(&level, UID, &command), SYS_CODE_INVALID_ARG));
}

static void test_condition(void)
{
    const ha_zb_state_record_t on = {.raw = 1, .zcl_type = HA_ZB_TYPE_BOOL};
    ha_automation_condition_t cond = {.op = HA_CONDITION_OP_EQ, .value = 1};
    CHECK(automation_rule_condition_ok(&cond, &on));
    cond.value = 0;
    CHECK(!automation_rule_condition_ok(&cond, &on));
    cond.op = HA_CONDITION_OP_NE;
    CHECK(automation_rule_condition_ok(&cond, &on));

    const ha_zb_state_record_t level = {.raw = 200, .zcl_type = HA_ZB_TYPE_UINT8};
    cond = (ha_automation_condition_t){.op = HA_CONDITION_OP_GE, .value = 100};
    CHECK(automation_rule_condition_ok(&cond, &level));
    cond.value = 250;
    CHECK(!automation_rule_condition_ok(&cond, &level));

    /* -5 °C: int16 приходит расширенным по знаку в raw */
    const ha_zb_state_record_t temp = {.raw = (uint32_t)(int32_t)-5, .zcl_type = HA_ZB_TYPE_INT16};
    cond = (ha_automation_condition_t){.op = HA_CONDITION_OP_LT, .value = 0};
    CHECK(automation_rule_condition_ok(&cond, &temp));
    cond.value = -10;
    CHECK(!automation_rule_condition_ok(&cond, &temp));

    float f = 21.5f;
    uint32_t bits = 0;
    memcpy(&bits, &f, sizeof(bits));
    const ha_zb_state_record_t fl = {.raw = bits, .zcl_type = HA_ZB_TYPE_SINGLE_FLOAT};
    cond = (ha_automation_condition_t){.op = HA_CONDITION_OP_EQ, .value = 21.5f};
    CHECK(automation_rule_condition_ok(&cond, &fl));

    const ha_zb_state_record_t str = {.raw = 0, .zcl_type = HA_ZB_TYPE_CHAR_STRING};
    cond = (ha_automation_condition_t){.op = HA_CONDITION_OP_EQ, .value = 0};
    CHECK(!automation_rule_condition_ok(&cond, &str));
    CHECK(!automation_rule_condition_ok(&cond, NULL));
    CHECK(!automation_rule_condition_ok(NULL, &on));

    cond = (ha_automation_condition_t){.op = 0, .value = 1};
    CHECK(!automation_rule_condition_ok(&cond, &on));

    /* HAS_BITS: маска Пн-Пт (0x1F) содержит Ср (бит 2), но не Сб (бит 5). */
    const ha_zb_state_record_t wmask = {.raw = 0x1Fu, .zcl_type = HA_ZB_TYPE_BITMAP8};
    cond = (ha_automation_condition_t){.op = HA_CONDITION_OP_HAS_BITS, .value = 0x04};
    CHECK(automation_rule_condition_ok(&cond, &wmask));
    cond.value = 0x20;
    CHECK(!automation_rule_condition_ok(&cond, &wmask));
}

static void test_time(void)
{
    ha_automation_record_t rule = {0};
    rule.enabled = 1;
    rule.trigger_kind = HA_TRIGGER_TIME;
    rule.trigger_a.time.minutes_of_day = 7 * 60; /* 07:00 */
    rule.trigger_a.time.weekday_mask = 0x1F;     /* Пн-Пт */

    CHECK(automation_rule_time_matches(&rule, 7 * 60, 1u << 0));   /* Пн 07:00 */
    CHECK(!automation_rule_time_matches(&rule, 7 * 60, 1u << 5));  /* Сб вне маски */
    CHECK(!automation_rule_time_matches(&rule, 8 * 60, 1u << 0));  /* не та минута */

    rule.enabled = 0;
    CHECK(!automation_rule_time_matches(&rule, 7 * 60, 1u << 0));

    /* Виды триггера не пересекаются. */
    ha_automation_record_t ev = {0};
    ev.enabled = 1;
    ev.trigger_kind = HA_TRIGGER_DEVICE_EVENT;
    CHECK(!automation_rule_time_matches(&ev, 0, 1));

    rule.enabled = 1;
    rule.trigger_kind = HA_TRIGGER_TIME;
    CHECK(!automation_rule_matches(&rule, 0x1234u, 2));
}

static void test_state(void)
{
    ha_automation_record_t rule = {0};
    rule.enabled = 1;
    rule.trigger_kind = HA_TRIGGER_STATE;
    rule.trigger_b.state.device_uid = UID;
    rule.trigger_b.state.endpoint = 2;
    rule.trigger_b.state.cluster_id = HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT;
    rule.trigger_b.state.attr_id = HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE;
    rule.trigger_b.state.op = HA_CONDITION_OP_LT;
    rule.trigger_a.state_value = 0.0f; /* < 0 */
    rule.trigger_b.state.edge = HA_TRIGGER_EDGE_RISING;

    const ha_zb_state_key_t key = {.device_uid = UID,
                                   .cluster_id = HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
                                   .attr_id = HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
                                   .endpoint = 2};

    /* стало истинно (5 -> -1) — срабатывает; уже истинно (-1 -> -2) — нет. */
    CHECK(automation_rule_state_matches(&rule, &key, -1.0, true, 5.0));
    CHECK(!automation_rule_state_matches(&rule, &key, -2.0, true, -1.0));
    /* стало ложно (-1 -> 3) — для RISING нет; первое наблюдение (-1) — да. */
    CHECK(!automation_rule_state_matches(&rule, &key, 3.0, true, -1.0));
    CHECK(automation_rule_state_matches(&rule, &key, -1.0, false, 0.0));

    /* другой endpoint/кластер — не совпадает. */
    ha_zb_state_key_t other = key;
    other.endpoint = 3;
    CHECK(!automation_rule_state_matches(&rule, &other, -1.0, true, 5.0));
    other = key;
    other.cluster_id = HA_ZB_CLUSTER_ON_OFF;
    CHECK(!automation_rule_state_matches(&rule, &other, -1.0, true, 5.0));

    /* FALLING: стало ложно. */
    rule.trigger_b.state.edge = HA_TRIGGER_EDGE_FALLING;
    CHECK(automation_rule_state_matches(&rule, &key, 3.0, true, -1.0));
    CHECK(!automation_rule_state_matches(&rule, &key, -2.0, true, -1.0));

    /* ANY: как только условие верно. */
    rule.trigger_b.state.edge = HA_TRIGGER_EDGE_ANY;
    CHECK(automation_rule_state_matches(&rule, &key, -2.0, true, -1.0));
    CHECK(!automation_rule_state_matches(&rule, &key, 2.0, true, -1.0));

    /* DEVICE_EVENT-правило не матчится как STATE. */
    ha_automation_record_t ev = {0};
    ev.enabled = 1;
    ev.trigger_kind = HA_TRIGGER_DEVICE_EVENT;
    CHECK(!automation_rule_state_matches(&ev, &key, -1.0, false, 0.0));
}

int main(void)
{
    test_matches();
    test_command();
    test_condition();
    test_time();
    test_state();

    if (g_failures != 0) {
        printf("test_rule: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_rule: OK\n");
    return 0;
}
