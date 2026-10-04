#include "automation/automation_rule.h"

#include <math.h>
#include <string.h>

#include "ha_model/ha_zigbee.h"

static sys_error_t automation_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_AUTOMATION, code);
}

#define CONDITION_EPS 1e-6

/*
 * ZCL-значение состояния → число. `raw` хранится по фактической ширине типа
 * (zigbee_radio.c:report_value): знаковые расширены по знаку, single float — биты.
 * Тип вне словаря скаляров не вычислить — условие считается невыполненным.
 */
static bool condition_value(const ha_zb_state_record_t *state, double *out)
{
    const uint32_t raw = state->raw;
    switch (state->zcl_type) {
    case HA_ZB_TYPE_BOOL:
    case HA_ZB_TYPE_BITMAP8:
    case HA_ZB_TYPE_UINT8:
    case HA_ZB_TYPE_ENUM8:
        *out = (double)(raw & 0xffu);
        return true;
    case HA_ZB_TYPE_INT8:
        *out = (double)(int8_t)(raw & 0xffu);
        return true;
    case HA_ZB_TYPE_UINT16:
    case HA_ZB_TYPE_ENUM16:
        *out = (double)(raw & 0xffffu);
        return true;
    case HA_ZB_TYPE_INT16:
        *out = (double)(int16_t)(raw & 0xffffu);
        return true;
    case HA_ZB_TYPE_UINT32:
        *out = (double)raw;
        return true;
    case HA_ZB_TYPE_INT32:
        *out = (double)(int32_t)raw;
        return true;
    case HA_ZB_TYPE_SINGLE_FLOAT: {
        float f = 0.0f;
        memcpy(&f, &raw, sizeof(f));
        *out = (double)f;
        return true;
    }
    default:
        return false;
    }
}

bool automation_rule_condition_ok(const ha_automation_condition_t *condition,
                                  const ha_zb_state_record_t *state)
{
    if (condition == NULL || state == NULL) {
        return false;
    }

    double actual = 0.0;
    if (!condition_value(state, &actual)) {
        return false;
    }
    const double expected = (double)condition->value;

    switch ((ha_condition_op_t)condition->op) {
    case HA_CONDITION_OP_EQ:
        return fabs(actual - expected) <= CONDITION_EPS;
    case HA_CONDITION_OP_NE:
        return fabs(actual - expected) > CONDITION_EPS;
    case HA_CONDITION_OP_GT:
        return actual > expected;
    case HA_CONDITION_OP_LT:
        return actual < expected;
    case HA_CONDITION_OP_GE:
        return actual >= expected;
    case HA_CONDITION_OP_LE:
        return actual <= expected;
    case HA_CONDITION_OP_HAS_BITS:
        return ((uint32_t)actual & (uint32_t)expected) != 0u;
    default:
        return false;
    }
}

bool automation_rule_matches(const ha_automation_record_t *rule, ha_device_uid_t device_uid,
                             uint16_t command_id)
{
    if (rule == NULL || rule->enabled == 0 ||
        rule->trigger_kind != (uint8_t)HA_TRIGGER_DEVICE_EVENT) {
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

bool automation_rule_time_matches(const ha_automation_record_t *rule, uint16_t minutes_of_day,
                                  uint8_t weekday_mask)
{
    if (rule == NULL || rule->enabled == 0 || rule->trigger_kind != (uint8_t)HA_TRIGGER_TIME) {
        return false;
    }
    if (rule->trigger_minutes_of_day != minutes_of_day) {
        return false;
    }
    return (rule->trigger_weekday_mask & weekday_mask) != 0;
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
