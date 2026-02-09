#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "gw_core/types.h"
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
} gw_device_t;

esp_err_t gw_device_registry_init(void);
esp_err_t gw_device_registry_upsert(const gw_device_t *device);
esp_err_t gw_device_registry_get(const gw_device_uid_t *uid, gw_device_t *out_device);
esp_err_t gw_device_registry_set_name(const gw_device_uid_t *uid, const char *name);
esp_err_t gw_device_registry_remove(const gw_device_uid_t *uid);
size_t gw_device_registry_list(gw_device_t *out_devices, size_t max_devices);

// New functions for endpoint management (backward compatible)
esp_err_t gw_device_registry_sync_endpoints(const gw_device_uid_t *uid);
size_t gw_device_registry_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);

#ifdef __cplusplus
}
#endif
