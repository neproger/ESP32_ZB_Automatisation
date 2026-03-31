// automation_store.c - Now using universal storage backend
#include "gw_core/automation_store.h"
#include "gw_core/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "gw_autos";

// Automation storage configuration
static const uint32_t AUTOMATION_STORAGE_MAGIC = 0x4155544f; // 'AUTO'
static const uint16_t AUTOMATION_STORAGE_VERSION = 2;
static const size_t AUTOMATION_STORAGE_MAX_ITEMS = 32;

static gw_storage_t s_automation_storage;
static bool s_initialized = false;
#define GW_AUTOMATION_LISTENER_CAP 4
typedef struct {
    gw_automation_store_listener_t cb;
    void *user_ctx;
} gw_automation_listener_slot_t;
static gw_automation_listener_slot_t s_listeners[GW_AUTOMATION_LISTENER_CAP];
static portMUX_TYPE s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

static void notify_automation_listeners(void)
{
    gw_automation_listener_slot_t listeners[GW_AUTOMATION_LISTENER_CAP];
    size_t listener_count = 0;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
        if (s_listeners[i].cb) {
            listeners[listener_count++] = s_listeners[i];
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < listener_count; i++) {
        listeners[i].cb(listeners[i].user_ctx);
    }
}

static bool automation_storage_ready(void)
{
    return s_initialized &&
           s_automation_storage.desc &&
           s_automation_storage.desc->key &&
           s_automation_storage.desc->key[0] != '\0' &&
           s_automation_storage.data != NULL;
}

static esp_err_t automation_storage_save_locked(void)
{
    if (!automation_storage_ready()) {
        ESP_LOGE(TAG,
                 "Automation storage not ready for save (init=%d desc=%p key=%p data=%p)",
                 s_initialized,
                 s_automation_storage.desc,
                 s_automation_storage.desc ? (const void *)s_automation_storage.desc->key : NULL,
                 s_automation_storage.data);
        return ESP_ERR_INVALID_STATE;
    }
    return gw_storage_save(&s_automation_storage);
}

// Storage descriptor
static const gw_storage_desc_t s_automation_storage_desc = {
    .key = "autos",
    .item_size = sizeof(gw_automation_entry_t),
    .max_items = AUTOMATION_STORAGE_MAX_ITEMS,
    .magic = AUTOMATION_STORAGE_MAGIC,
    .version = AUTOMATION_STORAGE_VERSION,
    .namespace = "autos"
};

// Internal helper functions
static size_t find_automation_index_by_id(const char *id);
static esp_err_t validate_automation_entry(const gw_automation_entry_t *entry);

esp_err_t gw_automation_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // S3 architecture now keeps automations in NVS only.
    esp_err_t err = gw_storage_init(&s_automation_storage, &s_automation_storage_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize automation storage (NVS): %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Automation storage initialized with %zu automations", s_automation_storage.count);
    return ESP_OK;
}

size_t gw_automation_store_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    const size_t count = s_automation_storage.count;
    portEXIT_CRITICAL(&s_automation_storage.lock);
    return count;
}

static size_t find_automation_index_by_id(const char *id)
{
    if (!id || !s_initialized) {
        return (size_t)-1;
    }

    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;
    for (size_t i = 0; i < s_automation_storage.count; i++) {
        if (strncmp(id, automations[i].id, GW_AUTOMATION_ID_MAX) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

size_t gw_automation_store_list(gw_automation_entry_t *out, size_t max_out)
{
    if (!s_initialized || !out || max_out == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    size_t count = s_automation_storage.count < max_out ? s_automation_storage.count : max_out;
    memcpy(out, s_automation_storage.data, count * sizeof(gw_automation_entry_t));
    portEXIT_CRITICAL(&s_automation_storage.lock);
    
    return count;
}

size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out)
{
    if (!s_initialized || !out || max_out == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;
    size_t count = s_automation_storage.count < max_out ? s_automation_storage.count : max_out;
    
    for (size_t i = 0; i < count; i++) {
        strlcpy(out[i].id, automations[i].id, sizeof(out[i].id));
        strlcpy(out[i].name, automations[i].name, sizeof(out[i].name));
        out[i].enabled = automations[i].enabled;
    }
    
    portEXIT_CRITICAL(&s_automation_storage.lock);
    return count;
}

esp_err_t gw_automation_store_get(const char *id, gw_automation_entry_t *out)
{
    if (!s_initialized || !id || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    size_t idx = find_automation_index_by_id(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_automation_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;
    *out = automations[idx];
    portEXIT_CRITICAL(&s_automation_storage.lock);
    
    return ESP_OK;
}

esp_err_t gw_automation_store_get_by_index(size_t index, gw_automation_entry_t *out)
{
    if (!s_initialized || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    if (index >= s_automation_storage.count) {
        portEXIT_CRITICAL(&s_automation_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }

    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;
    *out = automations[index];
    portEXIT_CRITICAL(&s_automation_storage.lock);
    return ESP_OK;
}

static esp_err_t validate_automation_entry(const gw_automation_entry_t *entry)
{
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }
    if (entry->id[0] == '\0' || entry->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(entry->id) >= GW_AUTOMATION_ID_MAX || strlen(entry->name) >= GW_AUTOMATION_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (entry->triggers_count > GW_AUTO_MAX_TRIGGERS ||
        entry->conditions_count > GW_AUTO_MAX_CONDITIONS ||
        entry->actions_count > GW_AUTO_MAX_ACTIONS ||
        entry->string_table_size > GW_AUTO_MAX_STRING_TABLE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t gw_automation_store_add_listener(gw_automation_store_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i].cb = cb;
            s_listeners[i].user_ctx = user_ctx;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t gw_automation_store_remove_listener(gw_automation_store_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            s_listeners[i].cb = NULL;
            s_listeners[i].user_ctx = NULL;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t gw_automation_store_put_entry(const gw_automation_entry_t *entry)
{
    if (!s_initialized || !entry) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = validate_automation_entry(entry);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);

    size_t idx = find_automation_index_by_id(entry->id);
    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;

    if (idx != (size_t)-1) {
        automations[idx] = *entry;
    } else {
        if (s_automation_storage.count >= AUTOMATION_STORAGE_MAX_ITEMS) {
            portEXIT_CRITICAL(&s_automation_storage.lock);
            return ESP_ERR_NO_MEM;
        }
        automations[s_automation_storage.count++] = *entry;
    }

    portEXIT_CRITICAL(&s_automation_storage.lock);
    err = automation_storage_save_locked();
    if (err == ESP_OK) notify_automation_listeners();
    return err;
}

esp_err_t gw_automation_store_remove(const char *id)
{
    if (!s_initialized || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    size_t idx = find_automation_index_by_id(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_automation_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Shift remaining automations down
    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;
    for (size_t i = idx + 1; i < s_automation_storage.count; i++) {
        automations[i - 1] = automations[i];
    }
    s_automation_storage.count--;
    memset(&automations[s_automation_storage.count], 0, sizeof(gw_automation_entry_t));
    
    portEXIT_CRITICAL(&s_automation_storage.lock);
    
    esp_err_t err = automation_storage_save_locked();
    if (err == ESP_OK) notify_automation_listeners();
    return err;
}

esp_err_t gw_automation_store_set_enabled(const char *id, bool enabled)
{
    if (!s_initialized || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    size_t idx = find_automation_index_by_id(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_automation_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;
    automations[idx].enabled = enabled;
    
    portEXIT_CRITICAL(&s_automation_storage.lock);

    esp_err_t err = automation_storage_save_locked();
    if (err == ESP_OK) notify_automation_listeners();
    return err;
}

esp_err_t gw_automation_store_remove_all(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    if (s_automation_storage.data && s_automation_storage.count > 0) {
        memset(s_automation_storage.data, 0, s_automation_storage.count * sizeof(gw_automation_entry_t));
    }
    s_automation_storage.count = 0;
    portEXIT_CRITICAL(&s_automation_storage.lock);

    esp_err_t err = automation_storage_save_locked();
    if (err == ESP_OK) notify_automation_listeners();
    return err;
}
