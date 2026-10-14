#pragma once

#include <stdbool.h>

#include "ha_model/ha_commands.h"
#include "ha_model/ha_rule.h"

/*
 * Чистая логика semantic-правила (Шаг A, A3.3): сопоставление одного триггера с
 * событием/состоянием и сборка semantic-команды одного действия. Работает только с
 * entity_id/property/action — не знает transport (uid/endpoint/cluster/attr/command).
 * Без FreeRTOS/Domain/логирования.
 *
 * SOURCE: HA_ENTITY_ID_SOURCE в триггере/действии/условии означает «источник триггера».
 * Резолвится в source_entity (для TIME-триггера engine передаёт ENTITY_SYSTEM).
 * `enabled` правила проверяет engine, а не эти функции.
 */

/* HA_ENTITY_ID_SOURCE → source_entity; иначе ref без изменений. */
ha_entity_id_t automation_rule_sem_resolve(ha_entity_id_t ref, ha_entity_id_t source_entity);

/* EVENT-триггер: source совпадает (или SOURCE), event_id равен. */
bool automation_rule_sem_event_matches(const ha_rule_trigger_t *trigger,
                                       ha_entity_id_t source_entity, ha_event_id_t event_id);

/* TIME-триггер: та же минута суток и хотя бы один общий день недели. */
bool automation_rule_sem_time_matches(const ha_rule_trigger_t *trigger, uint16_t minutes_of_day,
                                      uint8_t weekday_mask);

/*
 * STATE-триггер: entity (с учётом SOURCE) и property совпадают, условие
 * (op/value/value2) выполнено с учётом edge. prev_known=false — предыдущего нет.
 */
bool automation_rule_sem_state_matches(const ha_rule_trigger_t *trigger,
                                       ha_entity_id_t source_entity, ha_entity_id_t entity,
                                       ha_property_id_t property, double value, bool prev_known,
                                       double prev_value);

/* Оператор над значениями (общий для условии и STATE-триггера). */
bool automation_rule_sem_compare(uint8_t op, double lo, double hi, double actual);

/* Одно условие: применить оператор к уже прочитанному значению. */
bool automation_rule_sem_condition_ok(const ha_rule_condition_t *condition, double actual);

/*
 * Собрать semantic-команду одного действия (entity/property/action/value). false — цель
 * не резолвится (SOURCE без source_entity или NONE, неизвестное действие).
 */
bool automation_rule_sem_action(const ha_rule_action_t *action, ha_entity_id_t source_entity,
                                ha_property_command_t *out);
