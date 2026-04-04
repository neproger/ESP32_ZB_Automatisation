#include "gw_store/gw_store_hooks.h"

#if defined(__GNUC__)
#define GW_STORE_WEAK __attribute__((weak))
#else
#define GW_STORE_WEAK
#endif

GW_STORE_WEAK void gw_store_hook_remove_state_for_endpoint(const gw_store_endpoint_key_t *endpoint_key)
{
    (void)endpoint_key;
}

GW_STORE_WEAK esp_err_t gw_store_hook_notify_device_upsert(const gw_proto_device_v1_t *record)
{
    (void)record;
    return ESP_OK;
}

GW_STORE_WEAK esp_err_t gw_store_hook_notify_device_remove(const gw_device_uid_t *uid)
{
    (void)uid;
    return ESP_OK;
}

GW_STORE_WEAK esp_err_t gw_store_hook_notify_endpoint_upsert(const gw_proto_endpoint_v1_t *record)
{
    (void)record;
    return ESP_OK;
}

GW_STORE_WEAK esp_err_t gw_store_hook_notify_endpoint_remove(const gw_store_endpoint_key_t *key, uint16_t short_addr)
{
    (void)key;
    (void)short_addr;
    return ESP_OK;
}
