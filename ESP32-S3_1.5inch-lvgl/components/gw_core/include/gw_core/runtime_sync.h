#pragma once

#include "esp_err.h"
#include "gw_core/device_registry.h"
#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes runtime synchronization from incoming Zigbee events
// into local S3 stores (device registry, sensor/state cache).
esp_err_t gw_runtime_sync_init(void);

// Snapshot apply API (C6 -> S3 full sync).
esp_err_t gw_runtime_sync_snapshot_begin(uint16_t total_devices);
esp_err_t gw_runtime_sync_snapshot_upsert_device(const gw_device_t *device);
esp_err_t gw_runtime_sync_snapshot_upsert_endpoint(const gw_zb_endpoint_t *endpoint);
esp_err_t gw_runtime_sync_snapshot_remove_device(const gw_device_uid_t *uid);
esp_err_t gw_runtime_sync_snapshot_end(void);

#ifdef __cplusplus
}
#endif
