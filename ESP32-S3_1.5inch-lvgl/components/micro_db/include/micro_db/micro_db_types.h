#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generic embedded table store.
 *
 * This layer intentionally knows nothing about project entities.
 * It operates only on opaque records, opaque keys and table descriptors.
 */

typedef enum {
    MICRO_DB_BACKING_NONE = 0,
    MICRO_DB_BACKING_FLASH = 1 << 0,
    MICRO_DB_BACKING_RAM = 1 << 1,
} micro_db_backing_t;

typedef enum {
    MICRO_DB_TABLE_F_NONE = 0,
    MICRO_DB_TABLE_F_VERSIONED = 1 << 0,
} micro_db_table_flags_t;

typedef void (*micro_db_key_of_fn)(const void *record, void *out_key);
typedef bool (*micro_db_key_equals_fn)(const void *lhs_key, const void *rhs_key);
typedef bool (*micro_db_record_equals_fn)(const void *lhs_record, const void *rhs_record);

typedef struct {
    const char *name;
    size_t record_size;
    size_t key_size;
    size_t max_records;
    micro_db_backing_t backing;
    uint32_t flags;
    const char *persist_key; /* NULL when persistence is disabled */
    micro_db_key_of_fn key_of;
    micro_db_key_equals_fn key_equals;
    micro_db_record_equals_fn record_equals;
} micro_db_table_schema_t;

#ifdef __cplusplus
}
#endif
