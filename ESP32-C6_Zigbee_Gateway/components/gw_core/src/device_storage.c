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
        
        // Update device data
        memcpy(&devices[idx], device, sizeof(gw_device_full_t));
        
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

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_uid(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Shift remaining devices down
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    for (size_t i = idx + 1; i < s_device_storage.count; i++) {
        devices[i - 1] = devices[i];
    }
    s_device_storage.count--;
    memset(&devices[s_device_storage.count], 0, sizeof(gw_device_full_t));
    
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

esp_err_t gw_device_storage_add_endpoint(const gw_device_uid_t *uid, const gw_device_endpoint_t *endpoint)
{
    if (!s_initialized || !uid || !endpoint) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_uid(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    if (devices[idx].endpoint_count >= GW_DEVICE_MAX_ENDPOINTS) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NO_MEM;
    }
    
    devices[idx].endpoints[devices[idx].endpoint_count++] = *endpoint;
    
    portEXIT_CRITICAL(&s_device_storage.lock);
    return gw_storage_save(&s_device_storage);
}

esp_err_t gw_device_storage_remove_endpoint(const gw_device_uid_t *uid, uint8_t endpoint_num)
{
    if (!s_initialized || !uid) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_uid(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    bool found = false;
    
    for (size_t i = 0; i < devices[idx].endpoint_count; i++) {
        // Note: endpoint_num would need to be stored in the endpoint structure
        // For now, we'll just remove the last endpoint
        if (i == devices[idx].endpoint_count - 1) {
            found = true;
            devices[idx].endpoint_count--;
            memset(&devices[idx].endpoints[devices[idx].endpoint_count], 0, sizeof(gw_device_endpoint_t));
            break;
        }
    }
    
    portEXIT_CRITICAL(&s_device_storage.lock);
    
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    
    return gw_storage_save(&s_device_storage);
}

size_t gw_device_storage_get_endpoints(const gw_device_uid_t *uid, gw_device_endpoint_t *out_endpoints, size_t max_endpoints)
{
    if (!s_initialized || !uid || !out_endpoints || max_endpoints == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_uid(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return 0;
    }
    
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    size_t count = devices[idx].endpoint_count < max_endpoints ? devices[idx].endpoint_count : max_endpoints;
    memcpy(out_endpoints, devices[idx].endpoints, count * sizeof(gw_device_endpoint_t));
    
    portEXIT_CRITICAL(&s_device_storage.lock);
    return count;
}

// Legacy compatibility functions
esp_err_t gw_device_storage_get_legacy(const gw_device_uid_t *uid, gw_device_legacy_t *out_device)
{
    gw_device_full_t full_device;
    esp_err_t err = gw_device_storage_get(uid, &full_device);
    if (err != ESP_OK) {
        return err;
    }
    
    // Copy only the legacy fields
    out_device->device_uid = full_device.device_uid;
    out_device->short_addr = full_device.short_addr;
    strlcpy(out_device->name, full_device.name, sizeof(out_device->name));
    out_device->last_seen_ms = full_device.last_seen_ms;
    out_device->has_onoff = full_device.has_onoff;
    out_device->has_button = full_device.has_button;
    
    return ESP_OK;
}

size_t gw_device_storage_list_legacy(gw_device_legacy_t *out_devices, size_t max_devices)
{
    if (!s_initialized || !out_devices || max_devices == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t count = s_device_storage.count < max_devices ? s_device_storage.count : max_devices;
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    
    for (size_t i = 0; i < count; i++) {
        out_devices[i].device_uid = devices[i].device_uid;
        out_devices[i].short_addr = devices[i].short_addr;
        strlcpy(out_devices[i].name, devices[i].name, sizeof(out_devices[i].name));
        out_devices[i].last_seen_ms = devices[i].last_seen_ms;
        out_devices[i].has_onoff = devices[i].has_onoff;
        out_devices[i].has_button = devices[i].has_button;
    }
    
    portEXIT_CRITICAL(&s_device_storage.lock);
    return count;
}
