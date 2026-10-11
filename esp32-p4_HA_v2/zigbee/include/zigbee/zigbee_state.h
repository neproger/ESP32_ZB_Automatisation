#pragma once

#include "domain/domain.h"
#include "domain/domain_types.h"
#include "ha_model/ha_state.h"
#include "zigbee/zigbee_forms.h"
#include "sys/sys_error.h"
#include "zigbee/zigbee.h"

/*
 * Проекция репорта в canonical state. Здесь нет FreeRTOS и нет логирования, поэтому эта
 * часть сервиса собирается и тестируется на хосте (zigbee/tests).
 *
 * Правила (docs/services/ZIGBEE.md §3.1, §4, §9.4):
 *   - физический ключ состояния — та же координата, что в репорте;
 *   - значение хранится в кодировке ZCL, масштабирует его читатель;
 *   - в Domain (PROPERTY_STATE) пишет только тот, кто знает logical Entity.
 */

void zigbee_state_key_build(const zigbee_report_t *report, ha_zb_state_key_t *out_key);

/*
 * A1.2/A1.3: проекция репорта в canonical property state — адрес (entity_id, property_id)
 * и семантическое значение. false — атрибут вне семантического словаря или значение не
 * декодируется: смысл не выдумывается, canonical не пишется. Чистая функция (без
 * Domain/радио) — поэтому parity тестируется на хосте.
 */
bool zigbee_property_state_from_report(const zigbee_report_t *report, ha_entity_id_t entity_id,
                                       ha_state_key_t *out_key, ha_state_record_t *out_record);
