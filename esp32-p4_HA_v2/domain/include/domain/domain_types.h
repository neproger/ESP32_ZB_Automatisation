#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Публичные типы Domain.
 * Ошибки нейтральны: сервис не должен отличать слой хранения от фасада
 * (docs/domain/DOMAIN_API.md §3).
 */
typedef enum {
    DOMAIN_OK = 0,
    DOMAIN_NOT_FOUND,     /* сущность, ключ или тип не найден */
    DOMAIN_NO_SPACE,      /* место закончилось: слоты таблицы или registry */
    DOMAIN_INVALID_ARG,   /* аргумент не задан или вне диапазона */
    DOMAIN_INVALID_SIZE,  /* размер записи/ключа не совпадает с типом */
    DOMAIN_INVALID_STATE, /* тип уже зарегистрирован или Domain не инициализирован */
    DOMAIN_NO_MEM,
    DOMAIN_STALE,   /* ссылка на transient payload больше не действительна */
    DOMAIN_BUSY,
    DOMAIN_IO,
    DOMAIN_CORRUPT,
    DOMAIN_OVERFLOW,
} domain_err_t;

/* Storage policy задаётся в descriptor один раз при регистрации. */
typedef enum {
    DOMAIN_BACKING_NONE = 0,
    DOMAIN_BACKING_RAM = 1 << 0,
    DOMAIN_BACKING_FLASH = 1 << 1,
} domain_backing_t;

/* Дискриминатор типа сущности. Значения задаёт модель данных (ha_model),
 * Domain воспринимает их как opaque-число. */
typedef uint32_t domain_entity_t;

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
