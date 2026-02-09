#pragma once

#include "gw_core/storage.h"
#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Enhanced device structure that includes endpoints
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
    
    // Enhanced endpoint storage
    uint8_t endpoint_count;
    gw_device_endpoint_t endpoints[GW_DEVICE_MAX_ENDPOINTS];
} gw_device_full_t;

// Device storage operations
esp_err_t gw_device_storage_init(void);
esp_err_t gw_device_storage_upsert(const gw_device_full_t *device);
esp_err_t gw_device_storage_get(const gw_device_uid_t *uid, gw_device_full_t *out_device);
esp_err_t gw_device_storage_get_by_short(uint16_t short_addr, gw_device_full_t *out_device);
esp_err_t gw_device_storage_remove(const gw_device_uid_t *uid);
esp_err_t gw_device_storage_set_name(const gw_device_uid_t *uid, const char *name);
size_t gw_device_storage_list(gw_device_full_t *out_devices, size_t max_devices);

// Endpoint-specific operations
esp_err_t gw_device_storage_add_endpoint(const gw_device_uid_t *uid, const gw_device_endpoint_t *endpoint);
esp_err_t gw_device_storage_remove_endpoint(const gw_device_uid_t *uid, uint8_t endpoint_num);
size_t gw_device_storage_get_endpoints(const gw_device_uid_t *uid, gw_device_endpoint_t *out_endpoints, size_t max_endpoints);

// Legacy compatibility with existing device_registry_t
typedef struct {
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    char name[32];
    uint64_t last_seen_ms;
    bool has_onoff;
    bool has_button;
} gw_device_legacy_t;

esp_err_t gw_device_storage_get_legacy(const gw_device_uid_t *uid, gw_device_legacy_t *out_device);
size_t gw_device_storage_list_legacy(gw_device_legacy_t *out_devices, size_t max_devices);

#ifdef __cplusplus
}
#endif


