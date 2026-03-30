#include "gw_core/device_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "gw_device_storage";

// Storage configuration
static const uint32_t DEVICE_STORAGE_MAGIC = 0x44455653; // 'DEVS'
static const uint16_t DEVICE_STORAGE_VERSION = 1;
static const size_t DEVICE_STORAGE_MAX_DEVICES = GW_DEVICE_MAX_DEVICES;

static gw_storage_t s_device_storage;
static bool s_initialized = false;
static uint32_t s_version_seq = 0;

// Storage descriptor
static const gw_storage_desc_t s_device_storage_desc = {
    .key = "devices",
    .item_size = sizeof(gw_device_full_t),
    .max_items = DEVICE_STORAGE_MAX_DEVICES,
    .magic = DEVICE_STORAGE_MAGIC,
    .version = DEVICE_STORAGE_VERSION,
    .namespace = "gw"
};

// Internal helper functions
static size_t find_device_index_by_uid(const gw_device_uid_t *uid);
static size_t find_device_index_by_short(uint16_t short_addr);
static void assign_default_name_if_needed(gw_device_full_t *device);
static uint32_t next_version(void);

esp_err_t gw_device_storage_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = gw_storage_init(&s_device_storage, &s_device_storage_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device storage: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Device storage initialized with %zu devices", s_device_storage.count);
    return ESP_OK;
}

static uint32_t next_version(void)
{
    return ++s_version_seq;
}

static size_t find_device_index_by_uid(const gw_device_uid_t *uid)
{
    if (!uid || !s_initialized) {
        return (size_t)-1;
    }

    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (strncmp(uid->uid, devices[i].device_uid.uid, sizeof(uid->uid)) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static size_t find_device_index_by_short(uint16_t short_addr)
{
    if (!s_initialized) {
        return (size_t)-1;
    }

    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (devices[i].short_addr == short_addr) {
            return i;
        }
    }
    return (size_t)-1;
}

static void assign_default_name_if_needed(gw_device_full_t *device)
{
    if (!device || device->name[0] != '\0') {
        return; // Already has a name
    }

    const char *prefix = "device";
    if (device->has_button) {
        prefix = "switch";
    } else if (device->has_onoff) {
        prefix = "relay";
    }

    // Find next available number for this prefix
    uint32_t max_num = 0;
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (strncmp(devices[i].name, prefix, strlen(prefix)) == 0) {
            const char *num_str = devices[i].name + strlen(prefix);
            char *end;
            long num = strtol(num_str, &end, 10);
            if (end > num_str && num > 0 && num <= 999) {
                if ((uint32_t)num > max_num) {
                    max_num = (uint32_t)num;
                }
            }
        }
    }

    snprintf(device->name, sizeof(device->name), "%s%u", prefix, (unsigned)(max_num + 1));
}

esp_err_t gw_device_storage_upsert(const gw_device_full_t *device)
{
    if (!s_initialized || !device) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    
    size_t idx = find_device_index_by_uid(&device->device_uid);
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    
    if (idx != (size_t)-1) {
        // Update existing device in place
        const char *preserve_name = (device->name[0] == '\0') ? devices[idx].name : NULL;
        const uint32_t prev_version = devices[idx].version;
        const bool changed =
            (devices[idx].short_addr != device->short_addr) ||
            (strncmp(devices[idx].name, device->name, sizeof(devices[idx].name)) != 0 && device->name[0] != '\0') ||
            (devices[idx].last_seen_ms != device->last_seen_ms) ||
            (devices[idx].has_onoff != device->has_onoff) ||
            (devices[idx].has_button != device->has_button);
        
        // Update device data
        memcpy(&devices[idx], device, sizeof(gw_device_full_t));
        if (changed) {
            devices[idx].version = next_version();
        } else {
            devices[idx].version = prev_version;
        }
        
        // Restore name if needed
        if (preserve_name) {
            strlcpy(devices[idx].name, preserve_name, sizeof(devices[idx].name));
        }
        
        assign_default_name_if_needed(&devices[idx]);
        
        portEXIT_CRITICAL(&s_device_storage.lock);
        return gw_storage_save(&s_device_storage);
    }
    
    // Add new device
    if (s_device_storage.count >= DEVICE_STORAGE_MAX_DEVICES) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NO_MEM;
    }
    
    // Copy directly to storage array
    memcpy(&devices[s_device_storage.count], device, sizeof(gw_device_full_t));
    devices[s_device_storage.count].version = next_version();
    assign_default_name_if_needed(&devices[s_device_storage.count]);
    s_device_storage.count++;
    
    portEXIT_CRITICAL(&s_device_storage.lock);
    return gw_storage_save(&s_device_storage);
}

esp_err_t gw_device_storage_get(const gw_device_uid_t *uid, gw_device_full_t *out_device)
{
    if (!s_initialized || !uid || !out_device) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_uid(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    *out_device = devices[idx];
    portEXIT_CRITICAL(&s_device_storage.lock);
    
    return ESP_OK;
}

esp_err_t gw_device_storage_get_by_short(uint16_t short_addr, gw_device_full_t *out_device)
{
    if (!s_initialized || !out_device) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_short(short_addr);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    *out_device = devices[idx];
    portEXIT_CRITICAL(&s_device_storage.lock);
    
    return ESP_OK;
}

esp_err_t gw_device_storage_remove(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid) {
        return ESP_ERR_INVALID_ARG;
    }

    bool removed = false;

    portENTER_CRITICAL(&s_device_storage.lock);
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    size_t wr = 0;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (strncmp(uid->uid, devices[i].device_uid.uid, sizeof(uid->uid)) == 0) {
            removed = true;
            continue;
        }
        if (wr != i) {
            devices[wr] = devices[i];
        }
        wr++;
    }
    if (!removed) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = wr; i < s_device_storage.count; i++) {
        memset(&devices[i], 0, sizeof(gw_device_full_t));
    }
    s_device_storage.count = wr;
    portEXIT_CRITICAL(&s_device_storage.lock);
    return gw_storage_save(&s_device_storage);
}

esp_err_t gw_device_storage_set_name(const gw_device_uid_t *uid, const char *name)
{
    if (!s_initialized || !uid || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_uid(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    strlcpy(devices[idx].name, name, sizeof(devices[idx].name));
    devices[idx].version = next_version();
    
    portEXIT_CRITICAL(&s_device_storage.lock);
    return gw_storage_save(&s_device_storage);
}

size_t gw_device_storage_list(gw_device_full_t *out_devices, size_t max_devices)
{
    if (!s_initialized || !out_devices || max_devices == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t count = s_device_storage.count < max_devices ? s_device_storage.count : max_devices;
    memcpy(out_devices, s_device_storage.data, count * sizeof(gw_device_full_t));
    portEXIT_CRITICAL(&s_device_storage.lock);
    
    return count;
}

size_t gw_device_storage_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    size_t count = 0;
    portENTER_CRITICAL(&s_device_storage.lock);
    count = s_device_storage.count;
    portEXIT_CRITICAL(&s_device_storage.lock);
    return count;
}

esp_err_t gw_device_storage_get_by_index(size_t index, gw_device_full_t *out_device)
{
    if (!s_initialized || !out_device) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_device_storage.lock);
    if (index >= s_device_storage.count) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    *out_device = devices[index];
    portEXIT_CRITICAL(&s_device_storage.lock);
    return ESP_OK;
}

