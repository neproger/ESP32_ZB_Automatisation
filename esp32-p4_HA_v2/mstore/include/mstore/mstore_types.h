#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sys/sys_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mstore создаёт свою ошибку сам и больше ни одну не перекодирует: layer помечает
 * источник, смысл несёт общий code (docs/ERRORS.md).
 */
static inline sys_error_t mstore_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_MSTORE, code);
}

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
