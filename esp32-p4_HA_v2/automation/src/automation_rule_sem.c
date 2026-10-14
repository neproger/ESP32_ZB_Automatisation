#include "automation/automation_rule_sem.h"

#include <math.h>

#define CONDITION_EPS 1e-6

ha_entity_id_t automation_rule_sem_resolve(ha_entity_id_t ref, ha_entity_id_t source_entity)
{
    return (ref == HA_ENTITY_ID_SOURCE) ? source_entity : ref;
}

/* Применить оператор к паре чисел. */
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

bool automation_rule_sem_compare(uint8_t op, double lo, double hi, double actual)
{
    if ((ha_condition_op_t)op == HA_CONDITION_OP_BETWEEN) {
        return actual >= lo && actual <= hi;
    }
    return op_holds(op, actual, lo);
}

bool automation_rule_sem_condition_ok(const ha_rule_condition_t *condition, double actual)
{
    if (condition == NULL) {
        return false;
    }
    return automation_rule_sem_compare(condition->cmp.op, (double)condition->cmp.value,
                                       (double)condition->cmp.value2, actual);
}

bool automation_rule_sem_event_matches(const ha_rule_trigger_t *trigger,
                                       ha_entity_id_t source_entity, ha_event_id_t event_id)
{
    if (trigger == NULL || trigger->kind != (uint8_t)HA_TRIGGER_DEVICE_EVENT) {
        return false;
    }
    if (source_entity == HA_ENTITY_ID_NONE) {
        return false; /* источник неизвестен — правило не приписываем */
    }
    if (automation_rule_sem_resolve(trigger->entity, source_entity) != source_entity) {
        return false;
    }
    return trigger->event_id == event_id;
}

bool automation_rule_sem_time_matches(const ha_rule_trigger_t *trigger, uint16_t minutes_of_day,
                                      uint8_t weekday_mask)
{
    if (trigger == NULL || trigger->kind != (uint8_t)HA_TRIGGER_TIME) {
        return false;
    }
    if (trigger->minutes_of_day != minutes_of_day) {
        return false;
    }
    return (trigger->weekday_mask & weekday_mask) != 0;
}

bool automation_rule_sem_state_matches(const ha_rule_trigger_t *trigger,
                                       ha_entity_id_t source_entity, ha_entity_id_t entity,
                                       ha_property_id_t property, double value, bool prev_known,
                                       double prev_value)
{
    if (trigger == NULL || trigger->kind != (uint8_t)HA_TRIGGER_STATE) {
        return false;
    }
    if (entity == HA_ENTITY_ID_NONE) {
        return false; /* у изменения нет адреса — ветка не наша */
    }
    if (automation_rule_sem_resolve(trigger->entity, source_entity) != entity) {
        return false;
    }
    if (trigger->property != property) {
        return false;
    }

    const uint8_t op = trigger->op;
    const double lo = (double)trigger->value;
    const double hi = (double)trigger->value2;
    const bool holds = automation_rule_sem_compare(op, lo, hi, value);

    switch ((ha_automation_trigger_edge_t)trigger->edge) {
    case HA_TRIGGER_EDGE_RISING:
        return holds && (!prev_known || !automation_rule_sem_compare(op, lo, hi, prev_value));
    case HA_TRIGGER_EDGE_FALLING:
        return !holds && prev_known && automation_rule_sem_compare(op, lo, hi, prev_value);
    case HA_TRIGGER_EDGE_ANY:
    default:
        return holds;
    }
}

bool automation_rule_sem_action(const ha_rule_action_t *action, ha_entity_id_t source_entity,
                                ha_property_command_t *out)
{
    if (action == NULL || out == NULL) {
        return false;
    }
    const ha_entity_id_t target = automation_rule_sem_resolve(action->ref.entity, source_entity);
    if (target == HA_ENTITY_ID_NONE || target == HA_ENTITY_ID_SOURCE) {
        return false; /* цель не разрешилась (напр. SOURCE без источника) */
    }
    if ((ha_action_id_t)action->action == HA_ACTION_NONE ||
        action->ref.property == HA_PROPERTY_UNKNOWN) {
        return false;
    }

    ha_property_command_t command = {0};
    command.entity = target;
    command.property = action->ref.property;
    command.action = action->action;
    command.value = action->value;
    *out = command;
    return true;
}
