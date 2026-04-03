#include "gw_core/deleted_devices.h"

#include "gw_core/c6_store.h"

esp_err_t gw_deleted_devices_init(void)
{
    return gw_c6_store_init();
}

esp_err_t gw_deleted_devices_add(const gw_device_uid_t *uid, uint64_t removed_at_ms)
{
    return gw_c6_store_deleted_add(uid, removed_at_ms);
}

esp_err_t gw_deleted_devices_remove(const gw_device_uid_t *uid)
{
    return gw_c6_store_deleted_remove(uid);
}

bool gw_deleted_devices_contains(const gw_device_uid_t *uid)
{
    return gw_c6_store_deleted_contains(uid);
}

size_t gw_deleted_devices_count(void)
{
    return gw_c6_store_deleted_count();
}
