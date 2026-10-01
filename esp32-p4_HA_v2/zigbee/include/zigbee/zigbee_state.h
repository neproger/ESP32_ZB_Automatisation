#pragma once

#include "domain/domain.h"
#include "domain/domain_types.h"
#include "ha_model/ha_entities.h"
#include "zigbee/zigbee.h"

/*
 * Превращение репорта в запись Domain. Здесь нет FreeRTOS и нет логирования, поэтому
 * эта часть сервиса собирается и тестируется на хосте (zigbee/tests).
 *
 * Правила, которые здесь реализованы (docs/services/ZIGBEE.md §3.1, §4):
 *   - ключ состояния — та же координата, что в репорте;
 *   - значение хранится в кодировке ZCL, масштабирует его читатель;
 *   - устройство появляется в хранилище вместе с первым репортом.
 */

/*
 * Устройство появляется в хранилище вместе с первым репортом: Zigbee его уже назвал.
 * Идемпотентно: повторный вызов запись не множит.
 */
sys_error_t zigbee_device_ensure(domain_t *domain, ha_device_uid_t uid);

void zigbee_state_key_build(const zigbee_report_t *report, ha_zb_state_key_t *out_key);

/* Компактное значение факта: журнал не хранит запись целиком (JOURNAL.md §3). */
domain_value_t zigbee_value_compact(const zigbee_report_t *report);

/*
 * Записать состояние атрибута. out_changed — «состояние изменилось»: повтор того же
 * значения не даёт ни факта, ни изменения (ENTITY_STORE.md §8).
 */
sys_error_t zigbee_state_apply(domain_t *domain, const zigbee_report_t *report, bool *out_changed);
