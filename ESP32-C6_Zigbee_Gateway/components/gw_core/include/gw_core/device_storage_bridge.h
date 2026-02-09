#pragma once

#include "gw_core/device_storage.h"
#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bridge between zb_model and device_storage for endpoint persistence
esp_err_t gw_device_storage_bridge_init(void);

// Sync endpoints from zb_model to persistent storage
esp_err_t gw_device_storage_sync_endpoints(const gw_device_uid_t *uid);

// Load endpoints from persistent storage to zb_model
esp_err_t gw_device_storage_load_endpoints_to_zb_model(const gw_device_uid_t *uid);

// Get endpoints in zb_model format (for UI compatibility)
size_t gw_device_storage_get_zb_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);

#ifdef __cplusplus
}
#endif