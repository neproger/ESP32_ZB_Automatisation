#pragma once

/*
 * Generic Device service (Шаг B, docs/STEP_B_PLAN.md, B3'). Нейтральный слой над Domain:
 * принимает нормализованный ha_device_desc_t от adapter и превращает его в canonical
 * Domain identity (Logical Device + ownership Entity). Транспорта не знает; Domain — только
 * хранилище, adapter — источник физического представления.
 *
 * Граница зависимостей: UI/бизнес -> Device service -> Domain. Domain НЕ вызывает adapter;
 * adapter-specific removal инициируется через dispatcher выше (B7), а не отсюда.
 */

#include <stdint.h>

#include "domain/domain.h"
#include "ha_model/ha_entities.h" /* HA_ENTITY_ENTITY, HA_ENTITY_LOGICAL_DEVICE */
#include "ha_model/ha_entity.h"   /* ha_entity_id_t */
#include "ha_model/ha_logical_device.h"
#include "sys/sys_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Зарегистрировать (upsert) Logical Device из нормализованного описания. device_id и
 * parent_device_id выводятся generic-функцией; имя существующей записи не перетирается.
 * Идемпотентно. origin — DOMAIN_SOURCE_* (кто предоставил).
 */
sys_error_t device_register(domain_t *domain, uint8_t origin, const ha_device_desc_t *desc,
                            ha_device_id_t *out_device_id);

/*
 * Привязать Entity к Logical Device (ownership). Entity должна существовать (создаёт adapter);
 * иначе NOT_FOUND. Идемпотентно.
 */
sys_error_t device_bind_entity(domain_t *domain, uint8_t origin, ha_device_id_t device_id,
                               ha_entity_id_t entity_id);

/*
 * Domain-side удаление Logical Device: снять ownership у связанных Entity (device_id=NONE),
 * затем удалить запись Device. Физический unpair делает adapter через dispatcher (B7).
 */
sys_error_t device_remove(domain_t *domain, uint8_t origin, ha_device_id_t device_id);

/*
 * Adapter-specific remover: adapter регистрирует его по своему source. Device service зовёт
 * remover при удалении Logical Device; adapter forget/remove native-объект (для Zigbee —
 * leave в сеть «best effort», без ожидания подтверждения от физического устройства).
 * Возврат ошибки → canonical cleanup НЕ выполняется.
 */
typedef sys_error_t (*device_remover_fn)(const char *native_id, void *ctx);

sys_error_t device_register_remover(ha_source_id_t source, device_remover_fn fn, void *ctx);

/*
 * Переименовать Logical Device (только name; native_id/source/parent и Entity.name не трогает).
 * Device должна существовать → иначе NOT_FOUND.
 */
sys_error_t device_rename(domain_t *domain, uint8_t origin, ha_device_id_t device_id,
                          const char *name);

#ifdef __cplusplus
}
#endif
