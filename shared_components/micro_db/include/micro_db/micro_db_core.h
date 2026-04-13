#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "micro_db/micro_db_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const micro_db_table_schema_t *schema;
    void *records;
    uint8_t *slot_used;
    uint32_t *free_slots;
    uint32_t *primary_slots;
    uint8_t *primary_states;
    void *primary_keys;
    size_t primary_capacity;
    size_t live_count;
    size_t free_count;
    size_t capacity;
    void *lock;
    bool initialized;
} micro_db_table_t;

typedef struct {
    size_t live_count;
    size_t free_count;
    size_t capacity;
} micro_db_table_stats_t;

typedef bool (*micro_db_iter_cb_t)(const void *record, void *user_ctx);
typedef bool (*micro_db_iter_slot_cb_t)(uint32_t slot, const void *record, void *user_ctx);

esp_err_t micro_db_table_init(micro_db_table_t *table, const micro_db_table_schema_t *schema);
esp_err_t micro_db_table_deinit(micro_db_table_t *table);

esp_err_t micro_db_table_upsert(micro_db_table_t *table,
                                const void *record,
                                bool *out_changed,
                                bool *out_inserted);

esp_err_t micro_db_table_get(const micro_db_table_t *table,
                             const void *key,
                             void *out_record);

esp_err_t micro_db_table_remove(micro_db_table_t *table,
                                const void *key,
                                bool *out_removed);

esp_err_t micro_db_table_clear(micro_db_table_t *table);

size_t micro_db_table_count(const micro_db_table_t *table);
esp_err_t micro_db_table_get_stats(const micro_db_table_t *table, micro_db_table_stats_t *out_stats);

esp_err_t micro_db_table_get_slot(const micro_db_table_t *table,
                                  const void *key,
                                  uint32_t *out_slot);

esp_err_t micro_db_table_get_by_slot(const micro_db_table_t *table,
                                     uint32_t slot,
                                     void *out_record);

esp_err_t micro_db_table_get_by_index(const micro_db_table_t *table,
                                      size_t index,
                                      void *out_record);

size_t micro_db_table_iter(const micro_db_table_t *table,
                           micro_db_iter_cb_t cb,
                           void *user_ctx);

size_t micro_db_table_iter_slots(const micro_db_table_t *table,
                                 micro_db_iter_slot_cb_t cb,
                                 void *user_ctx);

#ifdef __cplusplus
}
#endif
