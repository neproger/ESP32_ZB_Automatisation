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

#include <stddef.h>
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

/*
 * Владелец identity — namespace/issuer, а не «transport» (id может создавать не только
 * физический транспорт). Значения стабильные, не переиспользуются.
 */
typedef enum {
    HA_ENTITY_ISSUER_ZIGBEE = 1,
    HA_ENTITY_ISSUER_SYSTEM = 2,
    HA_ENTITY_ISSUER_WEATHER = 3,
    HA_ENTITY_ISSUER_GPIO = 4,
} ha_entity_issuer_t;

/* Максимальный размер seed для derivation (Zigbee seed — 9 байт). */
#define HA_ENTITY_ID_DERIVE_SEED_MAX 64u

/*
 * Детерминированный вывод entity_id (Шаг A, A0.3): SipHash-2-4 с фиксированным
 * compile-time ключом проекта над каноническим входом
 *   LE32(issuer) || LE32(seed_len) || seed
 * Результат 0 (зарезервирован) детерминированно маппится в 1. Без random salt —
 * иначе теряется стабильность. Не зависит от порядка discovery/reboot.
 * false-случай: seed_len > HA_ENTITY_ID_DERIVE_SEED_MAX → HA_ENTITY_ID_NONE.
 */
ha_entity_id_t ha_entity_id_derive(uint32_t issuer, const void *seed, size_t seed_len);

/*
 * SipHash-2-4 (reference). Экспортируется как чистый стабильный примитив (используется
 * derivation; проверяется на официальном тест-векторе). key — 16 байт.
 */
uint64_t ha_siphash24(const uint8_t key[16], const void *data, size_t len);

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
