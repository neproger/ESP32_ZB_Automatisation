#include "gw_core/device_registry.h"
#include "gw_core/device_storage_bridge.h"

#include "esp_log.h"

esp_err_t gw_device_registry_init(void)
{
    return gw_device_storage_bridge_init();
}

esp_err_t gw_device_registry_upsert(const gw_device_t *device)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    // Convert legacy device to full device
    gw_device_full_t full_device = {0};
    const gw_device_legacy_t *legacy_device = (const gw_device_legacy_t*)device;
    full_device.device_uid = legacy_device->device_uid;
    full_device.short_addr = legacy_device->short_addr;
    strlcpy(full_device.name, legacy_device->name, sizeof(full_device.name));
    full_device.last_seen_ms = legacy_device->last_seen_ms;
    full_device.has_onoff = legacy_device->has_onoff;
    full_device.has_button = legacy_device->has_button;
    
    return gw_device_storage_upsert(&full_device);
}

esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    // Convert to legacy format for storage compatibility
    return gw_device_storage_get_legacy(uid, (gw_device_legacy_t*)out_device);
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
    // Convert to legacy format for storage compatibility
    return gw_device_storage_list_legacy((gw_device_legacy_t*)out_devices, max_devices);
}

esp_err_t gw_device_registry_sync_endpoints(const gw_device_uid_t *uid)
{
    return gw_device_storage_sync_endpoints(uid);
}

size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_device_storage_get_zb_endpoints(uid, out_eps, max_eps);
}
