#include "gw_core/device_registry.h"
#include "gw_core/device_storage_bridge.h"
#include "gw_core/zb_classify.h"

#include <stdlib.h>
#include <string.h>

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

esp_err_t gw_device_registry_init(void)
{
    return gw_device_storage_bridge_init();
}

esp_err_t gw_device_registry_upsert(const gw_device_t *device)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_full_t full_device = {0};
    full_device.device_uid = device->device_uid;
    full_device.short_addr = device->short_addr;
    strlcpy(full_device.name, device->name, sizeof(full_device.name));
    full_device.last_seen_ms = device->last_seen_ms;
    full_device.has_onoff = device->has_onoff;
    full_device.has_button = device->has_button;

    return gw_device_storage_upsert(&full_device);
}

esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    if (!uid || !out_device) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_device_full_t full_device = {0};
    esp_err_t err = gw_device_storage_get(uid, &full_device);
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
    derive_caps_from_topology(out_device);
    return ESP_OK;
}

esp_err_t gw_device_registry_set_name(const gw_device_uid_t *uid, const char *name)
{
    return gw_device_storage_set_name(uid, name);
}

esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid)
{
    return gw_device_storage_remove(uid);
}

size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices)
{
    if (!out_devices || max_devices == 0) {
        return 0;
    }
    const size_t cap = (max_devices < GW_DEVICE_MAX_DEVICES) ? max_devices : GW_DEVICE_MAX_DEVICES;
    gw_device_full_t *full_devices = (gw_device_full_t *)calloc(cap, sizeof(gw_device_full_t));
    if (!full_devices) {
        return 0;
    }

    size_t count = gw_device_storage_list(full_devices, cap);
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
        derive_caps_from_topology(&out_devices[i]);
    }
    free(full_devices);
    return count;
}

esp_err_t gw_device_registry_sync_endpoints(const gw_device_uid_t *uid)
{
    return gw_device_storage_sync_endpoints(uid);
}

size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_device_storage_get_zb_endpoints(uid, out_eps, max_eps);
}
