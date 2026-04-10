#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "gw_core/storage.h"
#include "gw_proto/gw_proto_types.h"
#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    char name[32];
    uint64_t last_seen_ms;
    bool has_onoff;
    bool has_button;
    gw_device_status_t status;
} gw_device_t;

#define GW_DEVICE_MAX_DEVICES 64
#define GW_DEVICE_MAX_ENDPOINTS 8
#define GW_DEVICE_MAX_CLUSTERS 16

typedef struct {
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t in_cluster_count;
    uint8_t out_cluster_count;
    uint16_t in_clusters[GW_DEVICE_MAX_CLUSTERS];
    uint16_t out_clusters[GW_DEVICE_MAX_CLUSTERS];
} gw_device_endpoint_t;

typedef struct {
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    char name[32];
    uint64_t last_seen_ms;
    bool has_onoff;
    bool has_button;
    gw_device_status_t status;
    uint8_t endpoint_count;
    gw_device_endpoint_t endpoints[GW_DEVICE_MAX_ENDPOINTS];
} gw_device_full_t;

esp_err_t gw_device_registry_init(void);
esp_err_t gw_device_registry_upsert(const gw_device_t *device);
esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device);
esp_err_t gw_device_registry_get_full(const gw_device_uid_t *uid, gw_device_full_t *out_device);
esp_err_t gw_device_registry_get_full_by_short(uint16_t short_addr, gw_device_full_t *out_device);
esp_err_t gw_device_registry_get_full_by_index(size_t index, gw_device_full_t *out_device);
esp_err_t gw_device_registry_remove_full(const gw_device_uid_t *uid);
esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid);
size_t gw_device_registry_count(void);
size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices);
size_t gw_device_registry_list_full(gw_device_full_t *out_devices, size_t max_devices);

esp_err_t gw_device_registry_sync_endpoints(const gw_device_uid_t *uid);
size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);

#ifdef __cplusplus
}
#endif
