#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#ifdef __cplusplus
extern "C" {
#endif

// Storage backend types
typedef enum {
    GW_STORAGE_NVS,     // Non-Volatile Storage - for critical data
    GW_STORAGE_SPIFFS   // SPIFFS filesystem - for complex/large data
} gw_storage_backend_t;

// Storage item descriptor
typedef struct {
    const char *key;                    // Unique key within namespace
    size_t item_size;                   // Size of single item
    size_t max_items;                   // Maximum number of items
    uint32_t magic;                     // Magic number for validation
    uint16_t version;                   // Version for compatibility
    const char *namespace;              // NVS namespace or SPIFFS directory
} gw_storage_desc_t;

// Generic storage operations
typedef struct {
    const gw_storage_desc_t *desc;
    gw_storage_backend_t backend;
    bool initialized;
    void *data;                         // In-memory cache
    size_t count;                       // Current item count
    portMUX_TYPE lock;                  // Thread safety
} gw_storage_t;

// Initialize storage system
esp_err_t gw_storage_init(gw_storage_t *storage, const gw_storage_desc_t *desc, gw_storage_backend_t backend);

// Generic CRUD operations
esp_err_t gw_storage_upsert(gw_storage_t *storage, const void *item);
esp_err_t gw_storage_get(gw_storage_t *storage, const char *key, void *out_item);
esp_err_t gw_storage_remove(gw_storage_t *storage, const char *key);
size_t gw_storage_list(gw_storage_t *storage, void *out_items, size_t max_items);
esp_err_t gw_storage_clear(gw_storage_t *storage);

// Batch operations
esp_err_t gw_storage_save(gw_storage_t *storage);  // Force persist to backend
esp_err_t gw_storage_load(gw_storage_t *storage);  // Reload from backend

// Utility functions
size_t gw_storage_count(gw_storage_t *storage);
bool gw_storage_is_full(gw_storage_t *storage);
esp_err_t gw_storage_find_by_index(gw_storage_t *storage, size_t index, void *out_item);

#ifdef __cplusplus
}
#endif