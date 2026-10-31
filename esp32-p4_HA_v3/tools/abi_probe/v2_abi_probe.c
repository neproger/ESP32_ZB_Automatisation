#include <stddef.h>

#include "ha_model/ha_groups.h"
#include "ha_model/ha_rule.h"

#define ABA(type, size, align)                    \
    _Static_assert(sizeof(type) == (size), #type " size"); \
    _Static_assert(_Alignof(type) == (align), #type " align")
#define OFF(type, field, at)                              \
    _Static_assert(offsetof(type, field) == (at), #type "." #field " offset")

ABA(ha_group_key_t, 8, 8);
OFF(ha_group_key_t, id, 0);
ABA(ha_group_record_t, 32, 1);
OFF(ha_group_record_t, title, 0);
ABA(ha_group_item_key_t, 16, 8);
OFF(ha_group_item_key_t, group_id, 0);
OFF(ha_group_item_key_t, entity, 8);
ABA(ha_group_item_record_t, 36, 2);
OFF(ha_group_item_record_t, order, 0);
OFF(ha_group_item_record_t, reserved, 2);
OFF(ha_group_item_record_t, title, 4);
ABA(ha_group_automation_key_t, 16, 8);
OFF(ha_group_automation_key_t, group_id, 0);
OFF(ha_group_automation_key_t, automation_id, 8);
ABA(ha_automation_key_t, 8, 8);
OFF(ha_automation_key_t, id, 0);

ABA(ha_value_t, 8, 4);
OFF(ha_value_t, kind, 0);
OFF(ha_value_t, value, 4);
ABA(ha_command_value_t, 12, 4);
OFF(ha_command_value_t, kind, 0);
OFF(ha_command_value_t, value, 4);
ABA(ha_rule_ref_t, 16, 8);
OFF(ha_rule_ref_t, entity, 0);
OFF(ha_rule_ref_t, property, 8);
OFF(ha_rule_ref_t, reserved, 12);
ABA(ha_rule_compare_t, 12, 4);
OFF(ha_rule_compare_t, op, 0);
OFF(ha_rule_compare_t, reserved, 1);
OFF(ha_rule_compare_t, value, 4);
OFF(ha_rule_compare_t, value2, 8);
ABA(ha_rule_condition_t, 32, 8);
OFF(ha_rule_condition_t, ref, 0);
OFF(ha_rule_condition_t, cmp, 16);
OFF(ha_rule_condition_t, reserved, 28);
ABA(ha_rule_trigger_t, 32, 8);
OFF(ha_rule_trigger_t, entity, 0);
OFF(ha_rule_trigger_t, event_id, 8);
OFF(ha_rule_trigger_t, property, 12);
OFF(ha_rule_trigger_t, value, 16);
OFF(ha_rule_trigger_t, value2, 20);
OFF(ha_rule_trigger_t, minutes_of_day, 24);
OFF(ha_rule_trigger_t, kind, 26);
OFF(ha_rule_trigger_t, op, 27);
OFF(ha_rule_trigger_t, edge, 28);
OFF(ha_rule_trigger_t, weekday_mask, 29);
OFF(ha_rule_trigger_t, reserved, 30);
ABA(ha_rule_action_t, 32, 8);
OFF(ha_rule_action_t, ref, 0);
OFF(ha_rule_action_t, value, 16);
OFF(ha_rule_action_t, action, 28);
OFF(ha_rule_action_t, reserved, 29);
ABA(ha_rule_t, 456, 8);
OFF(ha_rule_t, triggers, 0);
OFF(ha_rule_t, actions, 128);
OFF(ha_rule_t, conditions, 256);
OFF(ha_rule_t, enabled, 384);
OFF(ha_rule_t, trigger_count, 385);
OFF(ha_rule_t, action_count, 386);
OFF(ha_rule_t, conditions_count, 387);
OFF(ha_rule_t, reserved, 388);
OFF(ha_rule_t, name, 392);

int v2_abi_probe_translation_unit;
