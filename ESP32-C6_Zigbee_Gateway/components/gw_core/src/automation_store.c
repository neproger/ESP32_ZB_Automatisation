// automation_store.c - Now using universal storage backend
#include "gw_core/automation_store.h"
#include "gw_core/storage.h"
#include "gw_core/automation_compiled.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "gw_autos";

// Automation storage configuration
static const uint32_t AUTOMATION_STORAGE_MAGIC = 0x4155544f; // 'AUTO'
static const uint16_t AUTOMATION_STORAGE_VERSION = 2;
static const size_t AUTOMATION_STORAGE_MAX_ITEMS = 32;

static gw_storage_t s_automation_storage;
static bool s_initialized = false;

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
    .namespace = "autos" // For SPIFFS, this becomes a directory
};

// Internal helper functions
static size_t find_automation_index_by_id(const char *id);
static esp_err_t compile_automation_cbor(const uint8_t *buf, size_t len, gw_automation_entry_t *out_entry);

esp_err_t gw_automation_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = gw_storage_init(&s_automation_storage, &s_automation_storage_desc, GW_STORAGE_SPIFFS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize automation storage: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Automation storage initialized with %zu automations", s_automation_storage.count);
    return ESP_OK;
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

static esp_err_t compile_automation_cbor(const uint8_t *buf, size_t len, gw_automation_entry_t *out_entry)
{
    if (!buf || len == 0 || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_auto_compiled_t compiled = {0};
    char err_buf[256] = {0};

    esp_err_t err = gw_auto_compile_cbor(buf, len, &compiled, err_buf, sizeof(err_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to compile automation: %s", err_buf);
        return err;
    }

    // Convert to storage format
    memset(out_entry, 0, sizeof(*out_entry));

    if (compiled.hdr.automation_count > 0) {
        const gw_auto_bin_automation_v2_t *src_auto = &compiled.autos[0];
        const char *id = compiled.strings + src_auto->id_off;
        const char *name = compiled.strings + src_auto->name_off;

        // Copy metadata
        strlcpy(out_entry->id, id ? id : "", sizeof(out_entry->id));
        strlcpy(out_entry->name, name ? name : "", sizeof(out_entry->name));
        out_entry->enabled = src_auto->enabled ? true : false;

        // Copy arrays (limited by storage format)
        out_entry->triggers_count = src_auto->triggers_count < GW_AUTO_MAX_TRIGGERS ?
                                  src_auto->triggers_count : GW_AUTO_MAX_TRIGGERS;
        for (uint8_t i = 0; i < out_entry->triggers_count; i++) {
            out_entry->triggers[i] = compiled.triggers[src_auto->triggers_index + i];
        }

        out_entry->conditions_count = src_auto->conditions_count < GW_AUTO_MAX_CONDITIONS ?
                                   src_auto->conditions_count : GW_AUTO_MAX_CONDITIONS;
        for (uint8_t i = 0; i < out_entry->conditions_count; i++) {
            out_entry->conditions[i] = compiled.conditions[src_auto->conditions_index + i];
        }

        out_entry->actions_count = src_auto->actions_count < GW_AUTO_MAX_ACTIONS ?
                                 src_auto->actions_count : GW_AUTO_MAX_ACTIONS;
        for (uint8_t i = 0; i < out_entry->actions_count; i++) {
            out_entry->actions[i] = compiled.actions[src_auto->actions_index + i];
        }

        // Copy string table (truncated if needed)
        size_t string_size = compiled.hdr.strings_size < GW_AUTO_MAX_STRING_TABLE_BYTES ?
                           compiled.hdr.strings_size : GW_AUTO_MAX_STRING_TABLE_BYTES;
        out_entry->string_table_size = string_size;
        memcpy(out_entry->string_table, compiled.strings, string_size);
    }

    gw_auto_compiled_free(&compiled);
    return ESP_OK;
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

esp_err_t gw_automation_store_put_cbor(const uint8_t *buf, size_t len)
{
    if (!s_initialized || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_automation_entry_t compiled_entry;
    esp_err_t err = compile_automation_cbor(buf, len, &compiled_entry);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to compile automation: %s", esp_err_to_name(err));
        return err;
    }

    if (compiled_entry.id[0] == '\0' || compiled_entry.name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(compiled_entry.id) >= GW_AUTOMATION_ID_MAX || strlen(compiled_entry.name) >= GW_AUTOMATION_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_automation_storage.lock);
    
    size_t idx = find_automation_index_by_id(compiled_entry.id);
    gw_automation_entry_t *automations = (gw_automation_entry_t *)s_automation_storage.data;
    
    if (idx != (size_t)-1) {
        // Update existing automation
        automations[idx] = compiled_entry;
    } else {
        // Add new automation
        if (s_automation_storage.count >= AUTOMATION_STORAGE_MAX_ITEMS) {
            portEXIT_CRITICAL(&s_automation_storage.lock);
            return ESP_ERR_NO_MEM;
        }
        automations[s_automation_storage.count++] = compiled_entry;
    }
    
    portEXIT_CRITICAL(&s_automation_storage.lock);
    
    return automation_storage_save_locked();
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
    
    return automation_storage_save_locked();
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

    return automation_storage_save_locked();
}
