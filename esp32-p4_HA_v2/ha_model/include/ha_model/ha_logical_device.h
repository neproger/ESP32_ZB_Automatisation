#pragma once

/*
 * Logical Device (Шаг B, docs/STEP_B_PLAN.md): логический узел/вещь, которой принадлежат
 * Entity. Transport-agnostic identity: Domain адресует устройство как device_id; физическая
 * привязка (Zigbee EUI64) — в adapter binding, НЕ здесь.
 *
 * Отличие от adapter'ского transport device record (uid-keyed, diagnostics): Logical Device
 * существует для ownership/lifecycle/UI grouping, Entity остаётся единицей
 * properties/actions/events. Domain не знает EUI64.
 *
 * Это пассивная форма (структуры + числа), без поведения и без Zigbee.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque logical device identity. 0 зарезервировано как invalid/none. */
typedef uint64_t ha_device_id_t;

#define HA_DEVICE_ID_NONE ((ha_device_id_t)0u)

#define HA_LOGICAL_DEVICE_NAME_MAX 32

/* Ключ Logical Device: только логическая identity. */
typedef struct {
    ha_device_id_t id;
} ha_logical_device_key_t;

/* Имя — логическое свойство устройства (пользовательское), не транспорта. */
typedef struct {
    char name[HA_LOGICAL_DEVICE_NAME_MAX];
} ha_logical_device_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_device_id_t) == 8, "ha_device_id_t: неожиданный размер");
static_assert(sizeof(ha_logical_device_key_t) == 8, "ha_logical_device_key_t: неожиданный размер");
static_assert(offsetof(ha_logical_device_key_t, id) == 0, "ha_logical_device_key_t: layout");
static_assert(sizeof(ha_logical_device_record_t) == HA_LOGICAL_DEVICE_NAME_MAX,
              "ha_logical_device_record_t: размер");
#else
_Static_assert(sizeof(ha_device_id_t) == 8, "ha_device_id_t: неожиданный размер");
_Static_assert(sizeof(ha_logical_device_key_t) == 8, "ha_logical_device_key_t: неожиданный размер");
_Static_assert(offsetof(ha_logical_device_key_t, id) == 0, "ha_logical_device_key_t: layout");
_Static_assert(sizeof(ha_logical_device_record_t) == HA_LOGICAL_DEVICE_NAME_MAX,
               "ha_logical_device_record_t: размер");
#endif

#ifdef __cplusplus
}
#endif
