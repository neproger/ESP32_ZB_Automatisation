#pragma once

#include <stdbool.h>

#include "ha_model/ha_automation.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "sys/sys_error.h"

/*
 * Чистая логика правила (docs/services/AUTOMATION.md): сопоставление с событием и
 * сборка команды. Без FreeRTOS, Domain и логирования — проверяется на хосте.
 */

/* Сопоставить правило-событие (device, command). Выключенное/TIME-правило не подходит. */
bool automation_rule_matches(const ha_automation_record_t *rule, ha_device_uid_t device_uid,
                             uint16_t command_id);

/*
 * Сопоставить правило-«будильник» текущему локальному времени: та же минута суток и
 * хотя бы один общий день недели. minutes_of_day/weekday_mask берутся у системного девайса.
 */
bool automation_rule_time_matches(const ha_automation_record_t *rule, uint16_t minutes_of_day,
                                  uint8_t weekday_mask);

/*
 * Сопоставить STATE-правило изменению атрибута: тот же device/cluster/attr (endpoint 0 —
 * любой) и условие op/value с учётом edge. value — новое значение, prev — предыдущее
 * (prev_known=false — предыдущего нет).
 */
bool automation_rule_state_matches(const ha_automation_record_t *rule, const ha_zb_state_key_t *key,
                                   double value, bool prev_known, double prev_value);

/* Декодировать ZCL-значение состояния в число (общее для условий и STATE-триггера). */
bool automation_rule_state_value(const ha_zb_state_record_t *state, double *out);

/*
 * Собрать Zigbee-команду действия. trigger_uid подставляется, когда у правила
 * action_device_uid == 0 («то же устройство, что вызвало»).
 */
sys_error_t automation_rule_command(const ha_automation_record_t *rule,
                                    ha_device_uid_t trigger_uid, ha_zb_command_t *out);

/*
 * Проверить одно условие по состоянию атрибута: декодировать ZCL-значение по
 * `zcl_type` и применить оператор. false — условие не выполнено (в том числе тип
 * вне словаря скаляров): правило не срабатывает.
 */
bool automation_rule_condition_ok(const ha_automation_condition_t *condition,
                                  const ha_zb_state_record_t *state);
