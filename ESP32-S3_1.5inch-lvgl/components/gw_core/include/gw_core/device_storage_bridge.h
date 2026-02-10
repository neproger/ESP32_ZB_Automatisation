#pragma once

#include "gw_core/device_storage.h"
#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bridge between zb_model and device storage.
esp_err_t gw_device_storage_bridge_init(void);

// Keep endpoint model synchronized.
esp_err_t gw_device_storage_sync_endpoints(const gw_device_uid_t *uid);

// Endpoints in zb_model format.
size_t gw_device_storage_get_zb_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);

#ifdef __cplusplus
}
#endif
