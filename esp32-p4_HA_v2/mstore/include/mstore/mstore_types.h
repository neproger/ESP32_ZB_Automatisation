#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MSTORE_OK = 0,
    MSTORE_NOT_FOUND,      /* логический поиск по key не нашёл запись */
    MSTORE_STALE,          /* cached slot + generation больше не актуален */
    MSTORE_ALREADY_EXISTS, /* key уже занят в этой table */
    MSTORE_NO_SPACE,       /* свободных slots не осталось */
    MSTORE_INVALID_ARG,
    MSTORE_NO_MEM,
    MSTORE_INVALID_STATE,  /* table не инициализирована или уже инициализирована */
    MSTORE_INVALID_SIZE,
    MSTORE_INVARIANT_FAILED,
} mstore_err_t;

/* slot — физическое место в table, не долговечная identity. */
typedef uint32_t mstore_slot_t;

typedef struct {
    bool used;
    uint32_t generation;
    uint32_t version;
} mstore_meta_t;

/* Сравнивает два payload. NULL означает побайтовое сравнение payload_size. */
typedef bool (*mstore_payload_equals_fn)(const void *lhs, const void *rhs);

/* Storage policy; backing выбирается один раз при init(). */
typedef enum {
    MSTORE_BACKING_NONE = 0,
    MSTORE_BACKING_RAM = 1 << 0,
    MSTORE_BACKING_FLASH = 1 << 1,
} mstore_backing_t;

typedef struct {
    size_t capacity;
    size_t key_size;   /* key — opaque bytes фиксированной длины */
    size_t payload_size;
    mstore_payload_equals_fn payload_equals;

    mstore_backing_t backing; /* RAM / FLASH / RAM|FLASH */
    const char *persist_key;  /* stable persistent identity; нужен для FLASH */
} mstore_table_schema_t;

#ifdef __cplusplus
}
#endif
