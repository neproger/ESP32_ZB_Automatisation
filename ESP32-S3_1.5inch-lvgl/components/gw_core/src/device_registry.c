#include "gw_core/device_registry.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "gw_core/gw_proto_bus.h"
#include "gw_core/storage.h"
#include "gw_core/zb_classify.h"
#include "gw_proto/gw_proto_map.h"

static const char *TAG = "gw_device_registry";

static const uint32_t DEVICE_REGISTRY_MAGIC = 0x44455653;
static const uint16_t DEVICE_REGISTRY_VERSION = 1;
static const size_t DEVICE_REGISTRY_MAX_DEVICES = GW_DEVICE_MAX_DEVICES;

static gw_storage_t s_device_storage;
static bool s_initialized = false;
static uint32_t s_version_seq = 0;

static const gw_storage_desc_t s_device_storage_desc = {
    .key = "devices",
    .item_size = sizeof(gw_device_t),
    .max_items = DEVICE_REGISTRY_MAX_DEVICES,
    .magic = DEVICE_REGISTRY_MAGIC,
    .version = DEVICE_REGISTRY_VERSION,
    .namespace = "gw",
};

static size_t find_device_index_by_uid(const gw_device_uid_t *uid);
static void assign_default_name_if_needed(gw_device_t *device);
static uint32_t next_version(void);

static void notify_device_listeners(const gw_device_t *device, bool removed)
{
    if (device && device->device_uid.uid[0] != '\0') {
        if (removed) {
            gw_proto_device_remove_v1_t msg = {0};
            gw_proto_hdr_t hdr = {0};
            gw_proto_fill_device_remove(&msg, &device->device_uid);
            gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_DEVICE_REMOVE, sizeof(msg), 0);
            (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
        } else {
            gw_proto_device_v1_t msg = {0};
            gw_proto_hdr_t hdr = {0};
            gw_proto_fill_device(&msg, device);
            gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_DEVICE_UPSERT, sizeof(msg), 0);
            (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
        }
    }
}

static void derive_caps_from_topology(gw_device_t *device)
{
    if (!device || device->device_uid.uid[0] == '\0') {
        return;
    }

    gw_zb_endpoint_t eps[GW_ZB_MAX_ENDPOINTS] = {0};
    const size_t count = gw_zb_model_list_endpoints(&device->device_uid, eps, GW_ZB_MAX_ENDPOINTS);
    if (count == 0) {
        return;
    }

    bool any_button = false;
    bool any_actuator = false;
    for (size_t i = 0; i < count; i++) {
        const char *kind = gw_zb_endpoint_kind(&eps[i]);
        const bool is_button =
            (strcmp(kind, "switch") == 0) ||
            (strcmp(kind, "dimmer_switch") == 0);
        const bool is_actuator =
            (strcmp(kind, "relay") == 0) ||
            (strcmp(kind, "dimmable_light") == 0) ||
            (strcmp(kind, "color_light") == 0);

        if (is_button) {
            any_button = true;
        }
        if (is_actuator) {
            any_actuator = true;
        }
    }

    if (any_button) {
        device->has_button = true;
        if (!any_actuator) {
            device->has_onoff = false;
        }
    }
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

    gw_device_t *devices = (gw_device_t *)s_device_storage.data;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (strncmp(uid->uid, devices[i].device_uid.uid, sizeof(uid->uid)) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static void assign_default_name_if_needed(gw_device_t *device)
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
    gw_device_t *devices = (gw_device_t *)s_device_storage.data;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (strncmp(devices[i].name, prefix, strlen(prefix)) == 0) {
            const char *num_str = devices[i].name + strlen(prefix);
            char *end = NULL;
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

esp_err_t gw_device_registry_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = gw_storage_init(&s_device_storage, &s_device_storage_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device registry: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Device registry initialized with %zu devices", s_device_storage.count);
    return ESP_OK;
}

esp_err_t gw_device_registry_upsert(const gw_device_t *device)
{
    if (!s_initialized || !device) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);

    size_t idx = find_device_index_by_uid(&device->device_uid);
    gw_device_t *devices = (gw_device_t *)s_device_storage.data;

    if (idx != (size_t)-1) {
        const char *preserve_name = (device->name[0] == '\0') ? devices[idx].name : NULL;
        const uint32_t prev_version = devices[idx].version;
        const bool changed =
            (devices[idx].short_addr != device->short_addr) ||
            (strncmp(devices[idx].name, device->name, sizeof(devices[idx].name)) != 0 && device->name[0] != '\0') ||
            (devices[idx].last_seen_ms != device->last_seen_ms) ||
            (devices[idx].has_onoff != device->has_onoff) ||
            (devices[idx].has_button != device->has_button);

        memcpy(&devices[idx], device, sizeof(gw_device_t));
        if (changed) {
            devices[idx].version = next_version();
        } else {
            devices[idx].version = prev_version;
        }
        if (preserve_name) {
            strlcpy(devices[idx].name, preserve_name, sizeof(devices[idx].name));
        }
        assign_default_name_if_needed(&devices[idx]);

        portEXIT_CRITICAL(&s_device_storage.lock);
        esp_err_t err = gw_storage_save(&s_device_storage);
        if (err == ESP_OK) {
            gw_device_t out = devices[idx];
            derive_caps_from_topology(&out);
            notify_device_listeners(&out, false);
        }
        return err;
    }

    if (s_device_storage.count >= DEVICE_REGISTRY_MAX_DEVICES) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NO_MEM;
    }

    memcpy(&devices[s_device_storage.count], device, sizeof(gw_device_t));
    devices[s_device_storage.count].version = next_version();
    assign_default_name_if_needed(&devices[s_device_storage.count]);
    gw_device_t out = devices[s_device_storage.count];
    s_device_storage.count++;

    portEXIT_CRITICAL(&s_device_storage.lock);
    esp_err_t err = gw_storage_save(&s_device_storage);
    if (err == ESP_OK) {
        derive_caps_from_topology(&out);
        notify_device_listeners(&out, false);
    }
    return err;
}

esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device)
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

    gw_device_t *devices = (gw_device_t *)s_device_storage.data;
    *out_device = devices[idx];
    portEXIT_CRITICAL(&s_device_storage.lock);

    derive_caps_from_topology(out_device);
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

    gw_device_t *devices = (gw_device_t *)s_device_storage.data;
    strlcpy(devices[idx].name, name, sizeof(devices[idx].name));
    devices[idx].version = next_version();
    gw_device_t out = devices[idx];
    portEXIT_CRITICAL(&s_device_storage.lock);

    esp_err_t err = gw_storage_save(&s_device_storage);
    if (err == ESP_OK) {
        derive_caps_from_topology(&out);
        notify_device_listeners(&out, false);
    }
    return err;
}

esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_t removed = {0};
    bool have_removed = (gw_device_registry_get(uid, &removed) == ESP_OK);

    bool removed_ok = false;
    portENTER_CRITICAL(&s_device_storage.lock);
    gw_device_t *devices = (gw_device_t *)s_device_storage.data;
    size_t wr = 0;
    for (size_t i = 0; i < s_device_storage.count; i++) {
        if (strncmp(uid->uid, devices[i].device_uid.uid, sizeof(uid->uid)) == 0) {
            removed_ok = true;
            continue;
        }
        if (wr != i) {
            devices[wr] = devices[i];
        }
        wr++;
    }
    if (!removed_ok) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = wr; i < s_device_storage.count; i++) {
        memset(&devices[i], 0, sizeof(gw_device_t));
    }
    s_device_storage.count = wr;
    portEXIT_CRITICAL(&s_device_storage.lock);

    esp_err_t err = gw_storage_save(&s_device_storage);
    if (err == ESP_OK && have_removed) {
        notify_device_listeners(&removed, true);
    }
    return err;
}

esp_err_t gw_device_registry_get_by_index(size_t index, gw_device_t *out_device)
{
    if (!s_initialized || !out_device) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    if (index >= s_device_storage.count) {
        portEXIT_CRITICAL(&s_device_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }

    gw_device_t *devices = (gw_device_t *)s_device_storage.data;
    *out_device = devices[index];
    portEXIT_CRITICAL(&s_device_storage.lock);

    derive_caps_from_topology(out_device);
    return ESP_OK;
}

size_t gw_device_registry_count(void)
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

size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices)
{
    if (!s_initialized || !out_devices || max_devices == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_device_storage.lock);
    size_t count = s_device_storage.count < max_devices ? s_device_storage.count : max_devices;
    memcpy(out_devices, s_device_storage.data, count * sizeof(gw_device_t));
    portEXIT_CRITICAL(&s_device_storage.lock);

    for (size_t i = 0; i < count; i++) {
        derive_caps_from_topology(&out_devices[i]);
    }
    return count;
}

esp_err_t gw_device_registry_sync_endpoints(const gw_device_uid_t *uid)
{
    (void)uid;
    return ESP_OK;
}

size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_zb_model_list_endpoints(uid, out_eps, max_eps);
}
