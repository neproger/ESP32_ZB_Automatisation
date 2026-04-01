#include "gw_core/device_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "gw_device_registry";

static const uint32_t DEVICE_STORAGE_MAGIC = 0x44455653;
static const uint16_t DEVICE_STORAGE_VERSION = 1;
static const size_t DEVICE_STORAGE_MAX_DEVICES = GW_DEVICE_MAX_DEVICES;

static gw_storage_t s_device_storage;
static bool s_initialized = false;

static const gw_storage_desc_t s_device_storage_desc = {
    .key = "devices",
    .item_size = sizeof(gw_device_full_t),
    .max_items = DEVICE_STORAGE_MAX_DEVICES,
    .magic = DEVICE_STORAGE_MAGIC,
    .version = DEVICE_STORAGE_VERSION,
    .namespace = "gw",
};

static size_t find_device_index_by_uid(const gw_device_uid_t *uid);
static size_t find_device_index_by_short(uint16_t short_addr);
static void assign_default_name_if_needed(gw_device_full_t *device);
static bool uid_to_u64(const char *uid, uint64_t *out);
static bool uid_equals_str(const char *a, const char *b);
static bool merge_duplicate_into(gw_device_full_t *dst, const gw_device_full_t *src);
static bool dedupe_loaded_devices(void);
static bool slot_has_endpoint_payload(const gw_device_endpoint_t *ep);

static bool slot_has_endpoint_payload(const gw_device_endpoint_t *ep)
{
    if (ep == NULL) {
        return false;
    }
    return ep->profile_id != 0 ||
           ep->device_id != 0 ||
           ep->in_cluster_count != 0 ||
           ep->out_cluster_count != 0;
}

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

static bool uid_equals_str(const char *a, const char *b)
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
            if (!uid_equals_str(devices[i].device_uid.uid, devices[j].device_uid.uid)) {
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

static size_t find_device_index_by_uid(const gw_device_uid_t *uid)
{
    if (!uid || !s_initialized) {
        return (size_t)-1;
    }

    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (uid_equals_str(uid->uid, devices[i].device_uid.uid)) {
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
        return;
    }

    const char *prefix = "device";
    if (device->has_button) {
        prefix = "switch";
    } else if (device->has_onoff) {
        prefix = "relay";
    }

    uint32_t max_num = 0;
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (strncmp(devices[i].name, prefix, strlen(prefix)) == 0) {
            const char *num_str = devices[i].name + strlen(prefix);
            char *end = NULL;
            long num = strtol(num_str, &end, 10);
            if (end > num_str && num > 0 && num <= 999 && (uint32_t)num > max_num) {
                max_num = (uint32_t)num;
            }
        }
    }

    snprintf(device->name, sizeof(device->name), "%s%u", prefix, (unsigned)(max_num + 1));
}

esp_err_t gw_device_registry_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = gw_storage_init(&s_device_storage, &s_device_storage_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device registry storage: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    if (dedupe_loaded_devices()) {
        ESP_LOGW(TAG, "Deduplicated devices on load, persisting cleaned registry");
        (void)gw_storage_save(&s_device_storage);
    }
    ESP_LOGI(TAG, "Device registry initialized with %zu devices", s_device_storage.count);
    return ESP_OK;
}

esp_err_t gw_device_registry_upsert(const gw_device_t *device)
{
    if (!s_initialized || !device) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_full_t full_device = {0};
    gw_device_full_t existing = {0};
    if (gw_device_registry_get_full(&device->device_uid, &existing) == ESP_OK) {
        full_device.endpoint_count = existing.endpoint_count;
        memcpy(full_device.endpoints, existing.endpoints, sizeof(full_device.endpoints));
    }
    full_device.device_uid = device->device_uid;
    full_device.short_addr = device->short_addr;
    strlcpy(full_device.name, device->name, sizeof(full_device.name));
    full_device.last_seen_ms = device->last_seen_ms;
    full_device.has_onoff = device->has_onoff;
    full_device.has_button = device->has_button;

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t idx = find_device_index_by_uid(&device->device_uid);
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;

    if (idx != (size_t)-1) {
        char preserved_name[sizeof(devices[idx].name)] = {0};
        const bool need_preserve_name = (device->name[0] == '\0');
        if (need_preserve_name) {
            strlcpy(preserved_name, devices[idx].name, sizeof(preserved_name));
        }

        memcpy(&devices[idx], &full_device, sizeof(gw_device_full_t));
        if (need_preserve_name) {
            strlcpy(devices[idx].name, preserved_name, sizeof(devices[idx].name));
        }
        assign_default_name_if_needed(&devices[idx]);

        portEXIT_CRITICAL(&s_device_storage.lock);
        return gw_storage_save(&s_device_storage);
    }

    if (s_device_storage.count >= DEVICE_STORAGE_MAX_DEVICES) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NO_MEM;
    }

    memcpy(&devices[s_device_storage.count], &full_device, sizeof(gw_device_full_t));
    assign_default_name_if_needed(&devices[s_device_storage.count]);
    s_device_storage.count++;

    portEXIT_CRITICAL(&s_device_storage.lock);
    return gw_storage_save(&s_device_storage);
}

esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    if (!uid || !out_device) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_device_full_t full_device = {0};
    esp_err_t err = gw_device_registry_get_full(uid, &full_device);
    if (err != ESP_OK) {
        return err;
    }
    memset(out_device, 0, sizeof(*out_device));
    out_device->device_uid = full_device.device_uid;
    out_device->short_addr = full_device.short_addr;
    strlcpy(out_device->name, full_device.name, sizeof(out_device->name));
    out_device->last_seen_ms = full_device.last_seen_ms;
    out_device->has_onoff = full_device.has_onoff;
    out_device->has_button = full_device.has_button;
    return ESP_OK;
}

esp_err_t gw_device_registry_get_full(const gw_device_uid_t *uid, gw_device_full_t *out_device)
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

esp_err_t gw_device_registry_get_full_by_short(uint16_t short_addr, gw_device_full_t *out_device)
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

esp_err_t gw_device_registry_get_full_by_index(size_t index, gw_device_full_t *out_device)
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

esp_err_t gw_device_registry_set_name(const gw_device_uid_t *uid, const char *name)
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

esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid) {
        return ESP_ERR_INVALID_ARG;
    }

    bool removed = false;
    portENTER_CRITICAL(&s_device_storage.lock);
    gw_device_full_t *devices = (gw_device_full_t *)s_device_storage.data;
    size_t wr = 0;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (uid_equals_str(uid->uid, devices[i].device_uid.uid)) {
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

size_t gw_device_registry_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t count = s_device_storage.count;
    portEXIT_CRITICAL(&s_device_storage.lock);
    return count;
}

size_t gw_device_registry_list_full(gw_device_full_t *out_devices, size_t max_devices)
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

size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices)
{
    if (!out_devices || max_devices == 0) {
        return 0;
    }

    gw_device_full_t *full_devices = (gw_device_full_t *)calloc(GW_DEVICE_MAX_DEVICES, sizeof(gw_device_full_t));
    if (!full_devices) {
        return 0;
    }

    size_t count = gw_device_registry_list_full(full_devices, GW_DEVICE_MAX_DEVICES);
    if (count > max_devices) {
        count = max_devices;
    }
    for (size_t i = 0; i < count; i++) {
        out_devices[i].device_uid = full_devices[i].device_uid;
        out_devices[i].short_addr = full_devices[i].short_addr;
        strlcpy(out_devices[i].name, full_devices[i].name, sizeof(out_devices[i].name));
        out_devices[i].last_seen_ms = full_devices[i].last_seen_ms;
        out_devices[i].has_onoff = full_devices[i].has_onoff;
        out_devices[i].has_button = full_devices[i].has_button;
    }
    free(full_devices);
    return count;
}

esp_err_t gw_device_registry_sync_endpoints(const gw_device_uid_t *uid)
{
    if (!uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_full_t d = {0};
    esp_err_t err = gw_device_registry_get_full(uid, &d);
    if (err != ESP_OK) {
        return err;
    }

    gw_zb_endpoint_t live_eps[GW_DEVICE_MAX_ENDPOINTS] = {0};
    size_t live_count = gw_zb_model_list_endpoints(uid, live_eps, GW_DEVICE_MAX_ENDPOINTS);
    if (live_count == 0) {
        return ESP_OK;
    }

    memset(d.endpoints, 0, sizeof(d.endpoints));
    d.endpoint_count = 0;
    d.has_onoff = false;

    for (size_t i = 0; i < live_count; i++) {
        const gw_zb_endpoint_t *src = &live_eps[i];
        if (src->endpoint == 0 || src->endpoint > GW_DEVICE_MAX_ENDPOINTS) {
            continue;
        }
        size_t slot = (size_t)(src->endpoint - 1);
        gw_device_endpoint_t *dst = &d.endpoints[slot];
        dst->profile_id = src->profile_id;
        dst->device_id = src->device_id;
        dst->in_cluster_count = src->in_cluster_count > GW_DEVICE_MAX_CLUSTERS ? GW_DEVICE_MAX_CLUSTERS : src->in_cluster_count;
        dst->out_cluster_count = src->out_cluster_count > GW_DEVICE_MAX_CLUSTERS ? GW_DEVICE_MAX_CLUSTERS : src->out_cluster_count;
        if (dst->in_cluster_count > 0) {
            memcpy(dst->in_clusters, src->in_clusters, dst->in_cluster_count * sizeof(uint16_t));
        }
        if (dst->out_cluster_count > 0) {
            memcpy(dst->out_clusters, src->out_clusters, dst->out_cluster_count * sizeof(uint16_t));
        }
        if (src->endpoint > d.endpoint_count) {
            d.endpoint_count = src->endpoint;
        }
        for (size_t ci = 0; ci < dst->in_cluster_count; ci++) {
            if (dst->in_clusters[ci] == 0x0006) {
                d.has_onoff = true;
                break;
            }
        }
    }

    gw_device_t updated = {
        .device_uid = d.device_uid,
        .short_addr = d.short_addr,
        .last_seen_ms = d.last_seen_ms,
        .has_onoff = d.has_onoff,
        .has_button = d.has_button,
    };
    strlcpy(updated.name, d.name, sizeof(updated.name));
    return gw_device_registry_upsert(&updated);
}

size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    if (!uid || !out_eps || max_eps == 0) {
        return 0;
    }
    gw_device_full_t d = {0};
    if (gw_device_registry_get_full(uid, &d) != ESP_OK) {
        return 0;
    }

    size_t out_count = 0;
    size_t max_slots = d.endpoint_count > GW_DEVICE_MAX_ENDPOINTS ? GW_DEVICE_MAX_ENDPOINTS : d.endpoint_count;
    for (size_t slot = 0; slot < max_slots && out_count < max_eps; slot++) {
        const gw_device_endpoint_t *src = &d.endpoints[slot];
        if (!slot_has_endpoint_payload(src)) {
            continue;
        }
        gw_zb_endpoint_t *dst = &out_eps[out_count++];
        memset(dst, 0, sizeof(*dst));
        dst->uid = d.device_uid;
        dst->short_addr = d.short_addr;
        dst->endpoint = (uint8_t)(slot + 1);
        dst->profile_id = src->profile_id;
        dst->device_id = src->device_id;
        dst->in_cluster_count = src->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : src->in_cluster_count;
        dst->out_cluster_count = src->out_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : src->out_cluster_count;
        if (dst->in_cluster_count > 0) {
            memcpy(dst->in_clusters, src->in_clusters, dst->in_cluster_count * sizeof(uint16_t));
        }
        if (dst->out_cluster_count > 0) {
            memcpy(dst->out_clusters, src->out_clusters, dst->out_cluster_count * sizeof(uint16_t));
        }
    }
    return out_count;
}
