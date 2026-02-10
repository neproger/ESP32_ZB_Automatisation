#include "gw_core/device_registry.h"
#include "gw_core/device_storage_bridge.h"

#include <stdlib.h>
#include <string.h>

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
    gw_device_full_t existing = {0};
    if (gw_device_storage_get(&device->device_uid, &existing) == ESP_OK) {
        full_device.endpoint_count = existing.endpoint_count;
        memcpy(full_device.endpoints, existing.endpoints, sizeof(full_device.endpoints));
    }
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

    gw_device_full_t *full_devices = (gw_device_full_t *)calloc(GW_DEVICE_MAX_DEVICES, sizeof(gw_device_full_t));
    if (!full_devices) {
        return 0;
    }

    size_t count = gw_device_storage_list(full_devices, GW_DEVICE_MAX_DEVICES);
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
    return gw_device_storage_sync_endpoints(uid);
}

size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_device_storage_get_zb_endpoints(uid, out_eps, max_eps);
}
