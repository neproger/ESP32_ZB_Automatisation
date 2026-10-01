#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mstore/mstore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Table Store: фиксированный массив slots.
 * slot = meta + key + payload; canonical state живёт в slots.
 * key принадлежит storage, payload — caller'у.
 */
typedef struct {
    void *_state;
} mstore_table_t;

/*
 * Обход под lock: колбэк получает и сам key, поэтому вызывать API этой же table
 * внутри него не нужно (и нельзя — lock уже взят). Буферы key/payload действительны
 * только на время вызова.
 */
typedef bool (*mstore_iter_cb_t)(mstore_slot_t slot,
                                 const mstore_meta_t *meta,
                                 const void *key,
                                 const void *payload,
                                 void *ctx);

sys_error_t mstore_table_init(mstore_table_t *table, const mstore_table_schema_t *schema);
sys_error_t mstore_table_deinit(mstore_table_t *table);

sys_error_t mstore_table_slot_find(const mstore_table_t *table, const void *key,
                                    mstore_slot_t *out_slot);
sys_error_t mstore_table_slot_meta(const mstore_table_t *table, mstore_slot_t slot,
                                    mstore_meta_t *out_meta);
sys_error_t mstore_table_slot_read(const mstore_table_t *table, mstore_slot_t slot,
                                    mstore_meta_t *out_meta, void *out_key, void *out_payload);

sys_error_t mstore_table_slot_allocate(mstore_table_t *table, const void *key, const void *payload,
                                        mstore_slot_t *out_slot, uint32_t *out_generation);
sys_error_t mstore_table_slot_update(mstore_table_t *table, mstore_slot_t slot,
                                      uint32_t expected_generation, const void *payload,
                                      bool *out_changed);
sys_error_t mstore_table_slot_free(mstore_table_t *table, mstore_slot_t slot,
                                    uint32_t expected_generation);

sys_error_t mstore_table_iter(const mstore_table_t *table, mstore_iter_cb_t cb, void *ctx);
sys_error_t mstore_table_count(const mstore_table_t *table, size_t *out_count);
sys_error_t mstore_table_clear(mstore_table_t *table);

#ifdef __cplusplus
}
#endif
