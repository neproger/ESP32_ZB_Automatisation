#include "micro_db/micro_db_core.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "micro_db/micro_db_flash.h"

enum {
    INDEX_EMPTY = 0,
    INDEX_USED = 1,
    INDEX_TOMBSTONE = 2,
};

static const char *TAG = "micro_db";

static SemaphoreHandle_t table_mutex(const micro_db_table_t *table)
{
    return table ? (SemaphoreHandle_t)table->lock : NULL;
}

static esp_err_t table_lock(const micro_db_table_t *table)
{
    SemaphoreHandle_t m = table_mutex(table);
    if (m == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTakeRecursive(m, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void table_unlock(const micro_db_table_t *table)
{
    SemaphoreHandle_t m = table_mutex(table);
    if (m != NULL) {
        (void)xSemaphoreGiveRecursive(m);
    }
}

static uint32_t hash_bytes(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static size_t next_index_capacity(size_t max_records)
{
    size_t cap = 1;
    const size_t doubled = max_records * 2u;
    const size_t target = doubled > 0 ? doubled : 2u;
    while (cap < target) {
        cap <<= 1u;
    }
    return cap;
}

static int primary_index_find(const micro_db_table_t *table, const void *key, bool *out_found)
{
    const size_t mask = table->primary_capacity - 1u;
    const uint32_t hash = hash_bytes(key, table->schema->key_size);
    int first_tombstone = -1;

    for (size_t probe = 0; probe < table->primary_capacity; ++probe) {
        const size_t pos = (hash + probe) & mask;
        const uint8_t state = table->primary_states[pos];
        if (state == INDEX_EMPTY) {
            *out_found = false;
            return (first_tombstone >= 0) ? first_tombstone : (int)pos;
        }
        if (state == INDEX_TOMBSTONE) {
            if (first_tombstone < 0) {
                first_tombstone = (int)pos;
            }
            continue;
        }

        const uint8_t *key_base = (const uint8_t *)table->primary_keys;
        const void *stored_key = key_base + (pos * table->schema->key_size);
        if (table->schema->key_equals(stored_key, key)) {
            *out_found = true;
            return (int)pos;
        }
    }

    *out_found = false;
    return first_tombstone;
}

static void primary_index_clear(micro_db_table_t *table)
{
    memset(table->primary_states, 0, table->primary_capacity * sizeof(uint8_t));
    memset(table->primary_slots, 0, table->primary_capacity * sizeof(uint32_t));
    memset(table->primary_keys, 0, table->primary_capacity * table->schema->key_size);
}

static int find_record_slot(const micro_db_table_t *table, const void *key)
{
    if (!table || !table->initialized || !table->schema || !key) {
        return -1;
    }

    bool found = false;
    const int pos = primary_index_find(table, key, &found);
    if (!found || pos < 0) {
        return -1;
    }

    return (int)table->primary_slots[pos];
}

static void primary_index_insert(micro_db_table_t *table, const void *key, uint32_t slot)
{
    bool found = false;
    const int pos = primary_index_find(table, key, &found);
    if (pos < 0) {
        return;
    }

    uint8_t *key_base = (uint8_t *)table->primary_keys;
    memcpy(key_base + ((size_t)pos * table->schema->key_size), key, table->schema->key_size);
    table->primary_slots[pos] = slot;
    table->primary_states[pos] = INDEX_USED;
}

static void primary_index_remove(micro_db_table_t *table, const void *key)
{
    bool found = false;
    const int pos = primary_index_find(table, key, &found);
    if (!found || pos < 0) {
        return;
    }

    table->primary_states[pos] = INDEX_TOMBSTONE;
}

static esp_err_t rebuild_runtime_state(micro_db_table_t *table)
{
    if (!table || !table->initialized || !table->schema) {
        return ESP_ERR_INVALID_STATE;
    }

    primary_index_clear(table);
    table->live_count = 0;
    table->free_count = 0;

    uint8_t key_buf[128];
    if (table->schema->key_size > sizeof(key_buf)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *base = (uint8_t *)table->records;
    for (size_t slot = 0; slot < table->capacity; ++slot) {
        if (!table->slot_used[slot]) {
            table->free_slots[table->free_count++] = (uint32_t)slot;
            continue;
        }

        memset(key_buf, 0, sizeof(key_buf));
        table->schema->key_of(base + (slot * table->schema->record_size), key_buf);
        primary_index_insert(table, key_buf, (uint32_t)slot);
        table->live_count++;
    }

    return ESP_OK;
}

static bool table_is_flash_backed(const micro_db_table_t *table)
{
    return table && table->initialized && table->schema &&
           ((table->schema->backing & MICRO_DB_BACKING_FLASH) != 0) &&
           table->schema->persist_key != NULL;
}

static esp_err_t persist_slot(const micro_db_table_t *table, uint32_t slot)
{
    if (!table_is_flash_backed(table)) {
        return ESP_OK;
    }
    return micro_db_flash_write_slot(table, slot);
}

static esp_err_t persist_slot_used(const micro_db_table_t *table, uint32_t slot)
{
    if (!table_is_flash_backed(table)) {
        return ESP_OK;
    }
    return micro_db_flash_write_slot_used(table, slot);
}

static esp_err_t persist_meta(const micro_db_table_t *table)
{
    if (!table_is_flash_backed(table)) {
        return ESP_OK;
    }
    return micro_db_flash_write_meta(table);
}

static esp_err_t load_table_image(micro_db_table_t *table)
{
    if (!table || !table->initialized || !table->schema) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((table->schema->backing & MICRO_DB_BACKING_FLASH) == 0 || !table->schema->persist_key) {
        return ESP_OK;
    }

    esp_err_t err = micro_db_flash_load_table(table);
    if (err == ESP_OK) {
        err = rebuild_runtime_state(table);
    }
    return err;
}

esp_err_t micro_db_table_init(micro_db_table_t *table, const micro_db_table_schema_t *schema)
{
    if (!table || !schema || !schema->record_size || !schema->max_records ||
        !schema->key_size || !schema->key_of || !schema->key_equals || !schema->record_equals) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(table, 0, sizeof(*table));
    table->schema = schema;
    table->capacity = schema->max_records;
    table->primary_capacity = next_index_capacity(schema->max_records);
    table->lock = xSemaphoreCreateRecursiveMutex();
    if (table->lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if ((schema->backing & MICRO_DB_BACKING_RAM) == 0) {
        vSemaphoreDelete((SemaphoreHandle_t)table->lock);
        table->lock = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    table->records = calloc(schema->max_records, schema->record_size);
    table->slot_used = calloc(schema->max_records, sizeof(uint8_t));
    table->free_slots = calloc(schema->max_records, sizeof(uint32_t));
    table->primary_slots = calloc(table->primary_capacity, sizeof(uint32_t));
    table->primary_states = calloc(table->primary_capacity, sizeof(uint8_t));
    table->primary_keys = calloc(table->primary_capacity, schema->key_size);

    if (!table->records || !table->slot_used || !table->free_slots ||
        !table->primary_slots || !table->primary_states || !table->primary_keys) {
        (void)micro_db_table_deinit(table);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < schema->max_records; ++i) {
        table->free_slots[i] = (uint32_t)(schema->max_records - 1 - i);
    }

    table->free_count = schema->max_records;
    table->initialized = true;

    if ((schema->backing & MICRO_DB_BACKING_FLASH) != 0 && schema->persist_key) {
        esp_err_t load_err = load_table_image(table);
        if (load_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "loaded persisted table %s: records=%u/%u",
                     schema->name ? schema->name : "(unnamed)",
                     (unsigned)table->live_count,
                     (unsigned)table->capacity);
        } else if (load_err == ESP_ERR_INVALID_RESPONSE || load_err == ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(TAG,
                     "persist data corrupt for %s, clearing flash region",
                     schema->name ? schema->name : "(unnamed)");
            esp_err_t clear_err = micro_db_flash_clear_table(table);
            if (clear_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "failed to clear corrupt region for %s: %s",
                         schema->name ? schema->name : "(unnamed)",
                         esp_err_to_name(clear_err));
            }
        } else if (load_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "persist load failed for %s: %s",
                     schema->name ? schema->name : "(unnamed)",
                     esp_err_to_name(load_err));
        }
    }

    return ESP_OK;
}

esp_err_t micro_db_table_deinit(micro_db_table_t *table)
{
    if (!table) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t m = table_mutex(table);
    if (m) {
        (void)xSemaphoreTakeRecursive(m, portMAX_DELAY);
    }

    free(table->primary_keys);
    free(table->primary_states);
    free(table->primary_slots);
    free(table->free_slots);
    free(table->slot_used);
    free(table->records);
    if (m) {
        (void)xSemaphoreGiveRecursive(m);
        vSemaphoreDelete(m);
    }
    memset(table, 0, sizeof(*table));
    return ESP_OK;
}

esp_err_t micro_db_table_upsert(micro_db_table_t *table,
                                const void *record,
                                bool *out_changed,
                                bool *out_inserted)
{
    if (!table || !table->initialized || !record) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t lock_err = table_lock(table);
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    if (out_changed) {
        *out_changed = false;
    }
    if (out_inserted) {
        *out_inserted = false;
    }

    uint8_t key_buf[128];
    if (table->schema->key_size > sizeof(key_buf)) {
        table_unlock(table);
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(key_buf, 0, sizeof(key_buf));
    table->schema->key_of(record, key_buf);

    const int slot_idx = find_record_slot(table, key_buf);
    uint8_t *base = (uint8_t *)table->records;

    if (slot_idx >= 0) {
        void *slot = base + ((size_t)slot_idx * table->schema->record_size);
        const bool changed = !table->schema->record_equals(slot, record);
        memcpy(slot, record, table->schema->record_size);
        if (out_changed) {
            *out_changed = changed;
        }
        if (changed) {
            esp_err_t err = persist_slot(table, (uint32_t)slot_idx);
            if (err != ESP_OK) {
                table_unlock(table);
                return err;
            }
        }
        table_unlock(table);
        return ESP_OK;
    }

    if (table->free_count == 0) {
        table_unlock(table);
        return ESP_ERR_NO_MEM;
    }

    const uint32_t slot = table->free_slots[--table->free_count];
    void *dst = base + ((size_t)slot * table->schema->record_size);
    memcpy(dst, record, table->schema->record_size);
    table->slot_used[slot] = 1;
    table->live_count++;
    primary_index_insert(table, key_buf, slot);

    if (out_changed) {
        *out_changed = true;
    }
    if (out_inserted) {
        *out_inserted = true;
    }
    esp_err_t err = persist_slot(table, slot);
    if (err != ESP_OK) {
        table_unlock(table);
        return err;
    }
    err = persist_slot_used(table, slot);
    if (err != ESP_OK) {
        table_unlock(table);
        return err;
    }
    err = persist_meta(table);
    table_unlock(table);
    return err;
}

esp_err_t micro_db_table_get(const micro_db_table_t *table,
                             const void *key,
                             void *out_record)
{
    if (!table || !table->initialized || !key || !out_record) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t lock_err = table_lock(table);
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    const int slot = find_record_slot(table, key);
    if (slot < 0) {
        table_unlock(table);
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *base = (const uint8_t *)table->records;
    const void *record = base + ((size_t)slot * table->schema->record_size);
    memcpy(out_record, record, table->schema->record_size);
    table_unlock(table);
    return ESP_OK;
}

esp_err_t micro_db_table_remove(micro_db_table_t *table,
                                const void *key,
                                bool *out_removed)
{
    if (!table || !table->initialized || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t lock_err = table_lock(table);
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    if (out_removed) {
        *out_removed = false;
    }

    const int slot_idx = find_record_slot(table, key);
    if (slot_idx < 0) {
        table_unlock(table);
        return ESP_ERR_NOT_FOUND;
    }

    const uint32_t slot = (uint32_t)slot_idx;
    uint8_t *base = (uint8_t *)table->records;
    void *record = base + ((size_t)slot * table->schema->record_size);

    uint8_t key_buf[128];
    memset(key_buf, 0, sizeof(key_buf));
    table->schema->key_of(record, key_buf);
    primary_index_remove(table, key_buf);

    memset(record, 0, table->schema->record_size);
    table->slot_used[slot] = 0;
    table->free_slots[table->free_count++] = slot;
    table->live_count--;

    if (out_removed) {
        *out_removed = true;
    }
    esp_err_t err = persist_slot_used(table, slot);
    if (err != ESP_OK) {
        table_unlock(table);
        return err;
    }
    err = persist_meta(table);
    table_unlock(table);
    return err;
}

size_t micro_db_table_count(const micro_db_table_t *table)
{
    if (!table || !table->initialized) {
        return 0;
    }
    if (table_lock(table) != ESP_OK) {
        return 0;
    }
    const size_t count = table->live_count;
    table_unlock(table);
    return count;
}

esp_err_t micro_db_table_get_slot(const micro_db_table_t *table,
                                  const void *key,
                                  uint32_t *out_slot)
{
    if (!table || !table->initialized || !key || !out_slot) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t lock_err = table_lock(table);
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    const int slot = find_record_slot(table, key);
    if (slot < 0) {
        table_unlock(table);
        return ESP_ERR_NOT_FOUND;
    }

    *out_slot = (uint32_t)slot;
    table_unlock(table);
    return ESP_OK;
}

esp_err_t micro_db_table_get_by_slot(const micro_db_table_t *table,
                                     uint32_t slot,
                                     void *out_record)
{
    if (!table || !table->initialized || !out_record) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t lock_err = table_lock(table);
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (slot >= table->capacity || !table->slot_used[slot]) {
        table_unlock(table);
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *base = (const uint8_t *)table->records;
    const void *record = base + ((size_t)slot * table->schema->record_size);
    memcpy(out_record, record, table->schema->record_size);
    table_unlock(table);
    return ESP_OK;
}

esp_err_t micro_db_table_get_by_index(const micro_db_table_t *table,
                                      size_t index,
                                      void *out_record)
{
    if (!table || !table->initialized || !out_record) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t lock_err = table_lock(table);
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (index >= table->live_count) {
        table_unlock(table);
        return ESP_ERR_NOT_FOUND;
    }

    size_t seen = 0;
    const uint8_t *base = (const uint8_t *)table->records;
    for (size_t slot = 0; slot < table->capacity; ++slot) {
        if (!table->slot_used[slot]) {
            continue;
        }
        if (seen == index) {
            const void *record = base + (slot * table->schema->record_size);
            memcpy(out_record, record, table->schema->record_size);
            table_unlock(table);
            return ESP_OK;
        }
        seen++;
    }

    table_unlock(table);
    return ESP_ERR_NOT_FOUND;
}

size_t micro_db_table_iter(const micro_db_table_t *table,
                           micro_db_iter_cb_t cb,
                           void *user_ctx)
{
    if (!table || !table->initialized || !cb) {
        return 0;
    }
    if (table_lock(table) != ESP_OK) {
        return 0;
    }

    size_t visited = 0;
    const uint8_t *base = (const uint8_t *)table->records;
    for (size_t slot = 0; slot < table->capacity; ++slot) {
        if (!table->slot_used[slot]) {
            continue;
        }
        const void *record = base + (slot * table->schema->record_size);
        visited++;
        if (!cb(record, user_ctx)) {
            break;
        }
    }
    table_unlock(table);
    return visited;
}

size_t micro_db_table_iter_slots(const micro_db_table_t *table,
                                 micro_db_iter_slot_cb_t cb,
                                 void *user_ctx)
{
    if (!table || !table->initialized || !cb) {
        return 0;
    }
    if (table_lock(table) != ESP_OK) {
        return 0;
    }

    size_t visited = 0;
    const uint8_t *base = (const uint8_t *)table->records;
    for (size_t slot = 0; slot < table->capacity; ++slot) {
        if (!table->slot_used[slot]) {
            continue;
        }
        const void *record = base + (slot * table->schema->record_size);
        visited++;
        if (!cb((uint32_t)slot, record, user_ctx)) {
            break;
        }
    }
    table_unlock(table);
    return visited;
}
