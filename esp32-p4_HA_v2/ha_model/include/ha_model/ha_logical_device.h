#pragma once

/*
 * Logical Device (Шаг B, docs/STEP_B_PLAN.md): нормализованный Device graph, которым владеют
 * Entity. Adapter описывает внешний объект нормализованно (ha_device_desc_t); generic Device
 * service превращает его в canonical Domain identity. Domain хранит graph, транспорта не знает.
 *
 * Отличие от adapter'ского transport device record (uid-keyed, diagnostics): Logical Device
 * существует для ownership/lifecycle/UI grouping, Entity остаётся единицей
 * properties/actions/events.
 *
 * `native_id` — opaque внешняя identity (provenance), Domain её НЕ парсит. `source` указывает,
 * какой adapter её предоставил. Это НЕ transport addressing (EUI64/endpoint/cluster остаются
 * adapter-private).
 *
 * Пассивная форма (структуры + чистый derivation), без поведения и без Zigbee.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque logical device identity. 0 зарезервировано как invalid/none. */
typedef uint64_t ha_device_id_t;

#define HA_DEVICE_ID_NONE ((ha_device_id_t)0u)

/*
 * Adapter namespace id (opaque): 0 — invalid/none. Конкретные значения принадлежат
 * адаптерам (стабильные, не переиспользуются); generic core их не именует.
 */
typedef uint32_t ha_source_id_t;
#define HA_SOURCE_NONE ((ha_source_id_t)0)

#define HA_NATIVE_ID_MAX 32
#define HA_LOGICAL_DEVICE_NAME_MAX 32

/* Ключ Logical Device: только логическая identity. */
typedef struct {
    ha_device_id_t id;
} ha_logical_device_key_t;

/*
 * Запись Logical Device. Порядок полей выбран так, чтобы не было неявного padding (Domain
 * сравнивает записи побайтово): parent(8) + source(4) + native(32) + name(32) + reserved(4).
 * `reserved` — явный хвост до кратного 8, всегда нулевой.
 */
typedef struct {
    ha_device_id_t parent_device_id; /* HA_DEVICE_ID_NONE — root */
    ha_source_id_t source;
    char native_id[HA_NATIVE_ID_MAX];
    char name[HA_LOGICAL_DEVICE_NAME_MAX];
    uint8_t reserved[4];
} ha_logical_device_record_t;

/*
 * DTO, который adapter подаёт в Device service: adapter описывает свой нормализованный внешний
 * граф, не вычисляя внутренние связи Domain. `parent_native_id == ""` — root.
 */
typedef struct {
    ha_source_id_t source;
    char native_id[HA_NATIVE_ID_MAX];
    char parent_native_id[HA_NATIVE_ID_MAX];
    char name[HA_LOGICAL_DEVICE_NAME_MAX];
} ha_device_desc_t;

/*
 * Единственная derivation identity для Logical Device (никаких альтернативных формул в adapter).
 * SipHash-2-4, тем же примитивом, что entity_id. Пустой/native_id == NULL или слишком длинный
 * → HA_DEVICE_ID_NONE.
 */
ha_device_id_t ha_device_id_derive(ha_source_id_t source, const char *native_id);

#ifdef __cplusplus
static_assert(sizeof(ha_device_id_t) == 8, "ha_device_id_t: неожиданный размер");
static_assert(sizeof(ha_logical_device_key_t) == 8, "ha_logical_device_key_t: неожиданный размер");
static_assert(offsetof(ha_logical_device_key_t, id) == 0, "ha_logical_device_key_t: layout");
static_assert(sizeof(ha_logical_device_record_t) == 80, "ha_logical_device_record_t: размер");
static_assert(offsetof(ha_logical_device_record_t, source) == 8, "record: layout source");
static_assert(offsetof(ha_logical_device_record_t, native_id) == 12, "record: layout native_id");
static_assert(offsetof(ha_logical_device_record_t, name) == 44, "record: layout name");
static_assert(sizeof(ha_device_desc_t) == 100, "ha_device_desc_t: размер");
#else
_Static_assert(sizeof(ha_device_id_t) == 8, "ha_device_id_t: неожиданный размер");
_Static_assert(sizeof(ha_logical_device_key_t) == 8, "ha_logical_device_key_t: неожиданный размер");
_Static_assert(offsetof(ha_logical_device_key_t, id) == 0, "ha_logical_device_key_t: layout");
_Static_assert(sizeof(ha_logical_device_record_t) == 80, "ha_logical_device_record_t: размер");
_Static_assert(offsetof(ha_logical_device_record_t, source) == 8, "record: layout source");
_Static_assert(offsetof(ha_logical_device_record_t, native_id) == 12, "record: layout native_id");
_Static_assert(offsetof(ha_logical_device_record_t, name) == 44, "record: layout name");
_Static_assert(sizeof(ha_device_desc_t) == 100, "ha_device_desc_t: размер");
#endif

#ifdef __cplusplus
}
#endif
