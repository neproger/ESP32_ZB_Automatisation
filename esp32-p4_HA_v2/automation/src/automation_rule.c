#include "automation/automation_rule.h"

#include <math.h>
#include <string.h>

#include "semantics/semantics.h"

static sys_error_t automation_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_AUTOMATION, code);
}

#define CONDITION_EPS 1e-6

/* Применить оператор сравнения к двум числам (общий для условий и STATE-триггера). */
static bool op_holds(uint8_t op, double actual, double expected)
{
    switch ((ha_condition_op_t)op) {
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

bool automation_rule_condition_ok(const ha_automation_condition_t *condition,
                                  const ha_zb_state_record_t *state)
{
    if (condition == NULL) {
        return false;
    }
    /* physical key переносится как opaque-адрес; смысл значения даёт мост (semantics). */
    const ha_zb_state_key_t key = {.cluster_id = condition->cluster_id,
                                   .attr_id = condition->attr_id};
    ha_value_t value = {0};
    if (!semantics_state_value(&key, state, &value)) {
        return false;
    }
    double actual = 0.0;
    if (!semantics_value_to_double(&value, &actual)) {
        return false;
    }
    if ((ha_condition_op_t)condition->op == HA_CONDITION_OP_BETWEEN) {
        return actual >= (double)condition->value && actual <= (double)condition->value2;
    }
    return op_holds(condition->op, actual, (double)condition->value);
}

bool automation_rule_matches(const ha_automation_record_t *rule, ha_device_uid_t device_uid,
                             uint16_t command_id)
{
    if (rule == NULL || rule->enabled == 0 ||
        rule->trigger_kind != (uint8_t)HA_TRIGGER_DEVICE_EVENT) {
        return false;
    }
    if (rule->trigger_b.event.device_uid != 0 && rule->trigger_b.event.device_uid != device_uid) {
        return false;
    }
    if (rule->trigger_b.event.command_id != 0 && rule->trigger_b.event.command_id != command_id) {
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
    if (rule->trigger_a.time.minutes_of_day != minutes_of_day) {
        return false;
    }
    return (rule->trigger_a.time.weekday_mask & weekday_mask) != 0;
}

/*
 * STATE-триггер: атрибут (device/cluster/attr) изменился и условие (op/value) выполнено
 * с учётом edge. prev — предыдущее значение (для «стало истинно/ложно»); prev_known
 * false — предыдущего нет (первое наблюдение).
 */
bool automation_rule_state_matches(const ha_automation_record_t *rule, const ha_zb_state_key_t *key,
                                   double value, bool prev_known, double prev_value)
{
    if (rule == NULL || key == NULL || rule->enabled == 0 ||
        rule->trigger_kind != (uint8_t)HA_TRIGGER_STATE) {
        return false;
    }
    if (rule->trigger_b.state.device_uid == 0 ||
        rule->trigger_b.state.device_uid != key->device_uid) {
        return false;
    }
    if (rule->trigger_b.state.endpoint != 0 && rule->trigger_b.state.endpoint != key->endpoint) {
        return false;
    }
    if (rule->trigger_b.state.cluster_id != key->cluster_id ||
        rule->trigger_b.state.attr_id != key->attr_id) {
        return false;
    }

    const uint8_t op = rule->trigger_b.state.op;
    const double expected = (double)rule->trigger_a.state_value;
    const bool holds = op_holds(op, value, expected);

    switch ((ha_automation_trigger_edge_t)rule->trigger_b.state.edge) {
    case HA_TRIGGER_EDGE_RISING:
        return holds && (!prev_known || !op_holds(op, prev_value, expected));
    case HA_TRIGGER_EDGE_FALLING:
        return !holds && prev_known && op_holds(op, prev_value, expected);
    case HA_TRIGGER_EDGE_ANY:
    default:
        return holds;
    }
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
