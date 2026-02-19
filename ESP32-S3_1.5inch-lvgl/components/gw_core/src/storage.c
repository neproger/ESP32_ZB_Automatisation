#include "gw_core/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"

static const char *TAG = "gw_storage";

// Internal helper functions
static esp_err_t nvs_backend_save(gw_storage_t *storage);
static esp_err_t nvs_backend_load(gw_storage_t *storage);
static esp_err_t spiffs_backend_save(gw_storage_t *storage);
static esp_err_t spiffs_backend_load(gw_storage_t *storage);

esp_err_t gw_storage_init(gw_storage_t *storage, const gw_storage_desc_t *desc, gw_storage_backend_t backend)
{
    if (!storage || !desc || !desc->key || !desc->namespace) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(storage, 0, sizeof(*storage));
    storage->desc = desc;
    storage->backend = backend;
    storage->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    // Prefer PSRAM for storage caches; fall back to generic heap.
    storage->data = heap_caps_calloc(desc->max_items, desc->item_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage->data) {
        storage->data = heap_caps_calloc(desc->max_items, desc->item_size, MALLOC_CAP_8BIT);
    }
    if (!storage->data) {
        ESP_LOGE(TAG, "Failed to allocate memory for storage cache");
        return ESP_ERR_NO_MEM;
    }

    // Initialize backend-specific resources
    esp_err_t err = ESP_OK;
    if (backend == GW_STORAGE_SPIFFS) {
        // Mount SPIFFS if not already mounted
        const esp_vfs_spiffs_conf_t conf = {
            .base_path = "/data",
            .partition_label = "gw_data",
            .max_files = 4,
            .format_if_mount_failed = false,
        };
        err = esp_vfs_spiffs_register(&conf);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // ESP_ERR_INVALID_STATE means already mounted
            ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
            free(storage->data);
            return err;
        }
    }

    // Load existing data from backend
    switch (backend) {
        case GW_STORAGE_NVS:
            err = nvs_backend_load(storage);
            break;
        case GW_STORAGE_SPIFFS:
            err = spiffs_backend_load(storage);
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NOT_FOUND) {
        // First boot / empty storage is a valid state.
        storage->count = 0;
        memset(storage->data, 0, desc->max_items * desc->item_size);
        err = ESP_OK;
        ESP_LOGW(TAG, "No persisted data for %s, starting with empty storage", desc->key);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load storage data: %s", esp_err_to_name(err));
        free(storage->data);
        return err;
    }

    storage->initialized = true;
    ESP_LOGI(TAG, "Storage initialized: %s (%zu/%zu items)", desc->key, storage->count, desc->max_items);
    return ESP_OK;
}

static esp_err_t nvs_backend_save(gw_storage_t *storage)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(storage->desc->namespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    // Create blob header
    size_t blob_size = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + 
                       (storage->count * storage->desc->item_size);
    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    // Pack blob: magic + version + count + data
    size_t offset = 0;
    memcpy(blob + offset, &storage->desc->magic, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    memcpy(blob + offset, &storage->desc->version, sizeof(uint16_t));
    offset += sizeof(uint16_t);
    memcpy(blob + offset, &storage->count, sizeof(uint16_t));
    offset += sizeof(uint16_t);
    memcpy(blob + offset, storage->data, storage->count * storage->desc->item_size);

    err = nvs_set_blob(handle, storage->desc->key, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    free(blob);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_backend_load(gw_storage_t *storage)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(storage->desc->namespace, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t blob_size = 0;
    err = nvs_get_blob(handle, storage->desc->key, NULL, &blob_size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err; // No data found is OK
    }

    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(handle, storage->desc->key, blob, &blob_size);
    nvs_close(handle);

    if (err != ESP_OK) {
        free(blob);
        return err;
    }

    // Unpack blob: magic + version + count + data
    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    
    if (offset + sizeof(uint32_t) <= blob_size) {
        memcpy(&magic, blob + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
    }
    if (offset + sizeof(uint16_t) <= blob_size) {
        memcpy(&version, blob + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
    }
    if (offset + sizeof(uint16_t) <= blob_size) {
        memcpy(&storage->count, blob + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
    }

    // Validate magic and version
    if (magic != storage->desc->magic || version != storage->desc->version) {
        ESP_LOGW(TAG, "Storage magic/version mismatch (magic:0x%08x ver:%u, expected:0x%08x ver:%u), clearing data", 
                 magic, version, storage->desc->magic, storage->desc->version);
        free(blob);
        // Instead of failing, initialize empty storage
        storage->count = 0;
        memset(storage->data, 0, storage->desc->max_items * storage->desc->item_size);
        return ESP_OK; // Continue with empty storage
    }

    // Validate count
    if (storage->count > storage->desc->max_items) {
        ESP_LOGW(TAG, "Storage count exceeds max, ignoring data");
        free(blob);
        return ESP_ERR_INVALID_SIZE;
    }

    // Copy data
    size_t data_size = storage->count * storage->desc->item_size;
    if (offset + data_size <= blob_size) {
        memcpy(storage->data, blob + offset, data_size);
    }

    free(blob);
    ESP_LOGI(TAG, "NVS loaded %zu items for %s", storage->count, storage->desc->key);
    return ESP_OK;
}

static esp_err_t spiffs_backend_save(gw_storage_t *storage)
{
    if (!storage || !storage->desc || !storage->desc->key || storage->desc->key[0] == '\0') {
        ESP_LOGE(TAG,
                 "Invalid storage descriptor when saving SPIFFS blob (storage=%p desc=%p key=%p)",
                 storage,
                 storage ? storage->desc : NULL,
                 storage && storage->desc ? (const void *)storage->desc->key : NULL);
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage->data) {
        ESP_LOGE(TAG, "SPIFFS save: data pointer not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (storage->desc->max_items == 0 || storage->desc->item_size == 0) {
        ESP_LOGE(TAG, "SPIFFS save: descriptor metadata invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (storage->count > storage->desc->max_items) {
        ESP_LOGW(TAG,
                 "Storage count (%zu) exceeds max items (%zu); clamping before persist",
                 storage->count,
                 storage->desc->max_items);
        storage->count = storage->desc->max_items;
    }

    char path[256];
    snprintf(path, sizeof(path), "/data/%s.bin", storage->desc->key);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return ESP_FAIL;
    }

    // Create blob header
    size_t blob_size = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + 
                       (storage->count * storage->desc->item_size);
    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    // Pack blob: magic + version + count + data
    size_t offset = 0;
    memcpy(blob + offset, &storage->desc->magic, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    memcpy(blob + offset, &storage->desc->version, sizeof(uint16_t));
    offset += sizeof(uint16_t);
    memcpy(blob + offset, &storage->count, sizeof(uint16_t));
    offset += sizeof(uint16_t);
    memcpy(blob + offset, storage->data, storage->count * storage->desc->item_size);

    size_t written = fwrite(blob, 1, blob_size, f);
    fclose(f);
    free(blob);

    if (written != blob_size) {
        ESP_LOGE(TAG, "Incomplete write to file: %s", path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t spiffs_backend_load(gw_storage_t *storage)
{
    if (!storage || !storage->desc || !storage->desc->key || storage->desc->key[0] == '\0') {
        ESP_LOGE(TAG,
                 "Invalid storage descriptor when loading SPIFFS blob (storage=%p desc=%p key=%p)",
                 storage,
                 storage ? storage->desc : NULL,
                 storage && storage->desc ? (const void *)storage->desc->key : NULL);
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    snprintf(path, sizeof(path), "/data/%s.bin", storage->desc->key);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND; // File not found is OK
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t *blob = malloc(file_size);
    if (!blob) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_size = fread(blob, 1, file_size, f);
    fclose(f);

    if (read_size != (size_t)file_size) {
        free(blob);
        return ESP_FAIL;
    }

    // Unpack blob: magic + version + count + data
    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    
    if (offset + sizeof(uint32_t) <= read_size) {
        memcpy(&magic, blob + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
    }
    if (offset + sizeof(uint16_t) <= read_size) {
        memcpy(&version, blob + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
    }
    if (offset + sizeof(uint16_t) <= read_size) {
        memcpy(&storage->count, blob + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
    }

    // Validate magic and version
    if (magic != storage->desc->magic || version != storage->desc->version) {
        ESP_LOGW(TAG, "Storage magic/version mismatch (magic:0x%08x ver:%u, expected:0x%08x ver:%u), clearing data", 
                 magic, version, storage->desc->magic, storage->desc->version);
        free(blob);
        // Instead of failing, initialize empty storage
        storage->count = 0;
        memset(storage->data, 0, storage->desc->max_items * storage->desc->item_size);
        return ESP_OK; // Continue with empty storage
    }

    // Validate count
    if (storage->count > storage->desc->max_items) {
        ESP_LOGW(TAG, "Storage count exceeds max, ignoring data");
        free(blob);
        return ESP_ERR_INVALID_SIZE;
    }

    // Copy data
    size_t data_size = storage->count * storage->desc->item_size;
    if (offset + data_size <= read_size) {
        memcpy(storage->data, blob + offset, data_size);
    }

    free(blob);
    ESP_LOGI(TAG, "SPIFFS loaded %zu items for %s", storage->count, storage->desc->key);
    return ESP_OK;
}

esp_err_t gw_storage_save(gw_storage_t *storage)
{
    if (!storage || !storage->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (storage->backend) {
        case GW_STORAGE_NVS:
            return nvs_backend_save(storage);
        case GW_STORAGE_SPIFFS:
            return spiffs_backend_save(storage);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t gw_storage_load(gw_storage_t *storage)
{
    if (!storage || !storage->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (storage->backend) {
        case GW_STORAGE_NVS:
            return nvs_backend_load(storage);
        case GW_STORAGE_SPIFFS:
            return spiffs_backend_load(storage);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

// Note: Generic CRUD operations would need to be customized per data type
// since C doesn't have true generics. We'll create specialized versions for each use case.

