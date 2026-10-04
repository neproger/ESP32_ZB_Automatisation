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

/* Сопоставить правило событию (device, command). Выключенное правило не подходит. */
bool automation_rule_matches(const ha_automation_record_t *rule, ha_device_uid_t device_uid,
                             uint16_t command_id);

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
