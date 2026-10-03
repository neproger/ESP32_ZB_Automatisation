#include "automation/automation_rule.h"

#include <stdio.h>

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
    rule.trigger_device_uid = UID;
    rule.trigger_command_id = HA_ZB_CMD_ON_OFF_TOGGLE;
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

int main(void)
{
    test_matches();
    test_command();

    if (g_failures != 0) {
        printf("test_rule: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_rule: OK\n");
    return 0;
}
