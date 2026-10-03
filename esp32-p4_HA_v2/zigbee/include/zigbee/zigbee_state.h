#pragma once

#include "domain/domain.h"
#include "domain/domain_types.h"
#include "ha_model/ha_entities.h"
#include "zigbee/zigbee.h"

/*
 * Превращение репорта в запись Domain. Здесь нет FreeRTOS и нет логирования, поэтому
 * эта часть сервиса собирается и тестируется на хосте (zigbee/tests).
 *
 * Правила, которые здесь реализованы (docs/services/ZIGBEE.md §3.1, §4, §9.4):
 *   - ключ состояния — та же координата, что в репорте;
 *   - значение хранится в кодировке ZCL, масштабирует его читатель;
 *   - устройство попадает в Domain только после интервью; репорт до него — не факт.
 */

/*
 * Записать устройство с моделью из Basic-кластера, сохранив имя, заданное пользователем.
 * Идемпотентно: повторный вызов запись не множит. Вызывается по завершении интервью.
 */
sys_error_t zigbee_device_set_model(domain_t *domain, ha_device_uid_t uid, const char *model,
                                    bool *out_changed);

/* Создать запись устройства, если её ещё нет (для топологии при интервью). */
sys_error_t zigbee_device_ensure(domain_t *domain, ha_device_uid_t uid);

/*
 * Устройство ушло из сети: снять его записи — состояние, topology endpoint'ов и
 * само устройство. Идемпотентно: чего нет, то не мешает (docs/services/ZIGBEE.md §8).
 */
sys_error_t zigbee_device_remove(domain_t *domain, ha_device_uid_t uid);

void zigbee_state_key_build(const zigbee_report_t *report, ha_zb_state_key_t *out_key);

/* Компактное значение факта: журнал не хранит запись целиком (JOURNAL.md §3). */
domain_value_t zigbee_value_compact(const zigbee_report_t *report);

/*
 * Записать состояние атрибута. out_changed — «состояние изменилось»: повтор того же
 * значения не даёт ни факта, ни изменения (ENTITY_STORE.md §8). Репорт от устройства,
 * которого нет в Domain (интервью не завершено), — NOT_FOUND: факта не будет.
 */
sys_error_t zigbee_state_apply(domain_t *domain, const zigbee_report_t *report, bool *out_changed);
