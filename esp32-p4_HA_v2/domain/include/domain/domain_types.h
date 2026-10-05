#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sys/sys_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Публичные типы Domain.
 * Ошибка нейтральна: сервис сравнивает код и не отличает слой хранения от фасада
 * (docs/domain/DOMAIN_API.md §3, docs/ERRORS.md).
 */

/* Storage policy задаётся в descriptor один раз при регистрации. */
typedef enum {
    DOMAIN_BACKING_NONE = 0,
    DOMAIN_BACKING_RAM = 1 << 0,
    DOMAIN_BACKING_FLASH = 1 << 1,
} domain_backing_t;

/* Дискриминатор типа сущности. Значения задаёт модель данных (ha_model),
 * Domain воспринимает их как opaque-число. */
typedef uint32_t domain_entity_t;

/*
 * Ссылка на transient payload: opaque, как event_id. Разбирать её сервис не может —
 * только копировать и сравнивать (docs/domain/DOMAIN_API.md §4).
 */
typedef uint64_t domain_payload_ref_t;

/*
 * Ревизия записи: opaque, сервис только сравнивает «изменилось / нет»
 * (docs/domain/DOMAIN_API.md §4). Растёт только при реальном изменении записи;
 * поколение слота (generation) сюда не входит — это физика хранилища.
 */
typedef struct {
    uint32_t value;
} domain_entity_version_t;

typedef struct {
    domain_entity_version_t version;
} domain_entity_meta_t;

/* Дискриминатор команды: Domain знает только число и callback исполнителя. */
typedef uint32_t domain_command_t;

/* Кто инициировал факт. Domain смысла источника не знает — это подпись вызывающего. */
typedef enum {
    DOMAIN_SOURCE_ZIGBEE = 1,
    DOMAIN_SOURCE_UI = 2,
    DOMAIN_SOURCE_AUTOMATION = 3,
    DOMAIN_SOURCE_SYSTEM = 4,
} domain_source_t;

/*
 * Источник кодируется битом в маске подписки, поэтому идентификатор обязан быть
 * меньше 32. Это техническая граница механизма фильтрации, а не семантика: Domain
 * по-прежнему не знает, что означает конкретный source.
 */
#define DOMAIN_SOURCE_ID_MAX 32

/*
 * Компактное значение факта: то, без чего запись Journal со временем теряет смысл.
 * Строк тут нет намеренно — human-readable вид строит UI (docs/domain/JOURNAL.md §3).
 */
typedef enum {
    DOMAIN_VALUE_NONE = 0,
    DOMAIN_VALUE_BOOL = 1,
    DOMAIN_VALUE_I32 = 2,
    DOMAIN_VALUE_U32 = 3,
    DOMAIN_VALUE_F32 = 4,
    DOMAIN_VALUE_ENUM = 5,
} domain_value_type_t;

typedef struct {
    uint8_t type;
    uint8_t reserved[3];
    union {
        uint32_t u32;
        int32_t i32;
        float f32;
    } v;
} domain_value_t;

typedef struct {
    domain_entity_t type;
    size_t key_size;
    size_t payload_size;
    size_t capacity;
    domain_backing_t backing;

    /*
     * Stable persistent identity таблицы. Обязателен при FLASH-backing,
     * иначе NULL. Должен жить всё время жизни Domain (обычно литерал).
     */
    const char *persist_key;
} domain_entity_desc_t;

#ifdef __cplusplus
}
#endif
