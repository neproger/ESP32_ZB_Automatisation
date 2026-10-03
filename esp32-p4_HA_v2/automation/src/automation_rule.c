#include "automation/automation_rule.h"

#include <string.h>

static sys_error_t automation_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_AUTOMATION, code);
}

bool automation_rule_matches(const ha_automation_record_t *rule, ha_device_uid_t device_uid,
                             uint16_t command_id)
{
    if (rule == NULL || rule->enabled == 0) {
        return false;
    }
    if (rule->trigger_device_uid != 0 && rule->trigger_device_uid != device_uid) {
        return false;
    }
    if (rule->trigger_command_id != 0 && rule->trigger_command_id != command_id) {
        return false;
    }
    return true;
}

sys_error_t automation_rule_command(const ha_automation_record_t *rule,
                                    ha_device_uid_t trigger_uid, ha_zb_command_t *out)
{
    if (rule == NULL || out == NULL || rule->action_args_len > HA_ZB_COMMAND_ARGS_MAX) {
        return automation_fail(SYS_CODE_INVALID_ARG);
    }

    ha_zb_command_t command = {0};
    command.device_uid = (rule->action_device_uid != 0) ? rule->action_device_uid : trigger_uid;
    command.dst_endpoint = rule->action_endpoint;
    command.cluster_id = rule->action_cluster_id;
    command.command_id = rule->action_command_id;
    command.args_len = rule->action_args_len;
    memcpy(command.args, rule->action_args, rule->action_args_len);

    *out = command;
    return SYS_OK;
}
