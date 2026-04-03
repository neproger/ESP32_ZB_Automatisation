#include "gw_core/device_registry.h"

#include "gw_core/c6_store.h"

esp_err_t gw_device_registry_init(void)
{
    return gw_c6_store_init();
}

esp_err_t gw_device_registry_upsert(const gw_device_t *device)
{
    return gw_c6_store_device_upsert(device);
}

esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    return gw_c6_store_device_get(uid, out_device);
}

esp_err_t gw_device_registry_get_full(const gw_device_uid_t *uid, gw_device_full_t *out_device)
{
    return gw_c6_store_device_get_full(uid, out_device);
}

esp_err_t gw_device_registry_get_full_by_short(uint16_t short_addr, gw_device_full_t *out_device)
{
    return gw_c6_store_device_get_full_by_short(short_addr, out_device);
}

esp_err_t gw_device_registry_get_full_by_index(size_t index, gw_device_full_t *out_device)
{
    return gw_c6_store_device_get_full_by_index(index, out_device);
}

esp_err_t gw_device_registry_set_name(const gw_device_uid_t *uid, const char *name)
{
    return gw_c6_store_device_set_name(uid, name);
}

esp_err_t gw_device_registry_remove_full(const gw_device_uid_t *uid)
{
    return gw_c6_store_device_remove(uid);
}

esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid)
{
    return gw_device_registry_remove_full(uid);
}

size_t gw_device_registry_count(void)
{
    return gw_c6_store_device_count();
}

size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices)
{
    return gw_c6_store_device_list(out_devices, max_devices);
}

size_t gw_device_registry_list_full(gw_device_full_t *out_devices, size_t max_devices)
{
    return gw_c6_store_device_list_full(out_devices, max_devices);
}

esp_err_t gw_device_registry_sync_endpoints(const gw_device_uid_t *uid)
{
    return gw_c6_store_device_sync_endpoints(uid);
}

size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_c6_store_endpoint_list(uid, out_eps, max_eps);
}
