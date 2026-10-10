#pragma once

/*
 * Логическая сущность (Шаг A, docs/STEP_A_PLAN.md §3). Transport-agnostic identity:
 * Domain адресует состояние как (entity_id, property_id); физический адрес
 * (Zigbee uid/endpoint/cluster/attr, GPIO num, Matter node) — в adapter binding, НЕ здесь.
 *
 * В этом заголовке НЕТ device_uid/endpoint/cluster/attr и зависимостей от ha_zigbee.h.
 * Связь entity_id ↔ transport binding (A0.2) и вывод entity_id из binding (A0.3) —
 * отдельные шаги; здесь их сознательно нет.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque stable logical identity. 0 зарезервировано как invalid/none.
 * Значение выводит adapter из persistent binding детерминированно (A0.3);
 * структура значения наружу не раскрывается.
 */
typedef uint64_t ha_entity_id_t;

#define HA_ENTITY_ID_NONE ((ha_entity_id_t)0u)

/* Ключ сущности в Entity Store: только логическая identity. */
typedef struct {
    ha_entity_id_t id;
} ha_entity_key_t;

/*
 * Минимальная transport-independent metadata. Сейчас её нет: запись содержит лишь
 * explicit reserved (по docs/RECORD_MODEL.md запись не может быть нулевого размера).
 * Реальные поля появятся, только когда возникнет потребность.
 */
typedef struct {
    uint8_t reserved;
} ha_entity_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_entity_id_t) == 8, "ha_entity_id_t: неожиданный размер");
static_assert(sizeof(ha_entity_key_t) == 8, "ha_entity_key_t: неожиданный размер");
static_assert(sizeof(ha_entity_record_t) == 1, "ha_entity_record_t: неожиданный размер");
#else
_Static_assert(sizeof(ha_entity_id_t) == 8, "ha_entity_id_t: неожиданный размер");
_Static_assert(sizeof(ha_entity_key_t) == 8, "ha_entity_key_t: неожиданный размер");
_Static_assert(sizeof(ha_entity_record_t) == 1, "ha_entity_record_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
