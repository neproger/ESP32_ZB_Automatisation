#include "gw_core/device_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
static bool uid_to_u64(const char *uid, uint64_t *out);
static bool uid_equals(const char *a, const char *b);
static bool merge_duplicate_into(gw_device_full_t *dst, const gw_device_full_t *src);
static bool dedupe_loaded_devices(void);

static bool uid_to_u64(const char *uid, uint64_t *out)
{
    if (!uid || !out) {
        return false;
    }
    const char *p = uid;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (*p == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 16);
    if (!end || *end != '\0') {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static bool uid_equals(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    uint64_t va = 0;
    uint64_t vb = 0;
    if (uid_to_u64(a, &va) && uid_to_u64(b, &vb)) {
        return va == vb;
    }
    return strcasecmp(a, b) == 0;
}

static bool merge_duplicate_into(gw_device_full_t *dst, const gw_device_full_t *src)
{
    if (!dst || !src) {
        return false;
    }
    bool changed = false;
    if (src->last_seen_ms > dst->last_seen_ms) {
        dst->last_seen_ms = src->last_seen_ms;
        changed = true;
    }
    if (dst->name[0] == '\0' && src->name[0] != '\0') {
        strlcpy(dst->name, src->name, sizeof(dst->name));
        changed = true;
    }
    if (!dst->has_onoff && src->has_onoff) {
        dst->has_onoff = true;
        changed = true;
    }
    if (!dst->has_button && src->has_button) {
        dst->has_button = true;
        changed = true;
    }
    if (src->short_addr != 0 && dst->short_addr == 0) {
        dst->short_addr = src->short_addr;
        changed = true;
    }
    if (src->endpoint_count > dst->endpoint_count) {
        dst->endpoint_count = src->endpoint_count;
        changed = true;
    }
    for (size_t i = 0; i < GW_DEVICE_MAX_ENDPOINTS; i++) {
        const gw_device_endpoint_t *se = &src->endpoints[i];
        gw_device_endpoint_t *de = &dst->endpoints[i];
        if (de->profile_id == 0 && se->profile_id != 0) {
            *de = *se;
            changed = true;
        }
    }
    return changed;
}

static bool dedupe_loaded_devices(void)
{
    if (!s_initialized || s_device_storage.count < 2) {
        return false;
    }

    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    bool changed = false;
    size_t i = 0;
    while (i < s_device_storage.count) {
        size_t j = i + 1;
        while (j < s_device_storage.count) {
            if (!uid_equals(devices[i].device_uid.uid, devices[j].device_uid.uid)) {
                j++;
                continue;
            }

            (void)merge_duplicate_into(&devices[i], &devices[j]);

            for (size_t k = j + 1; k < s_device_storage.count; k++) {
                devices[k - 1] = devices[k];
            }
            s_device_storage.count--;
            memset(&devices[s_device_storage.count], 0, sizeof(gw_device_full_t));
            changed = true;
        }
        i++;
    }

    return changed;
}

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
    if (dedupe_loaded_devices()) {
        ESP_LOGW(TAG, "Deduplicated devices on load, persisting cleaned registry");
        (void)gw_storage_save(&s_device_storage);
    }
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
        if (uid_equals(uid->uid, devices[i].device_uid.uid)) {
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
        char preserved_name[sizeof(devices[idx].name)] = {0};
        const bool need_preserve_name = (device->name[0] == '\0');
        if (need_preserve_name) {
            strlcpy(preserved_name, devices[idx].name, sizeof(preserved_name));
        }
        
        // Update device data
        memcpy(&devices[idx], device, sizeof(gw_device_full_t));
        
        // Restore name if needed
        if (need_preserve_name) {
            strlcpy(devices[idx].name, preserved_name, sizeof(devices[idx].name));
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

