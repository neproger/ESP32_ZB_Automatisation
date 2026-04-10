#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/device_registry.h"
#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t uid_num;
    uint8_t endpoint;
} gw_c6_endpoint_key_t;

esp_err_t gw_c6_store_init(void);

esp_err_t gw_c6_store_device_upsert(const gw_device_t *device);
esp_err_t gw_c6_store_device_get(const gw_device_uid_t *uid, gw_device_t *out_device);
esp_err_t gw_c6_store_device_get_full(const gw_device_uid_t *uid, gw_device_full_t *out_device);
esp_err_t gw_c6_store_device_get_full_by_short(uint16_t short_addr, gw_device_full_t *out_device);
esp_err_t gw_c6_store_device_get_full_by_index(size_t index, gw_device_full_t *out_device);
esp_err_t gw_c6_store_device_set_status(const gw_device_uid_t *uid, gw_device_status_t status);
esp_err_t gw_c6_store_device_remove(const gw_device_uid_t *uid);
size_t gw_c6_store_device_count(void);
size_t gw_c6_store_device_list(gw_device_t *out_devices, size_t max_devices);
size_t gw_c6_store_device_list_full(gw_device_full_t *out_devices, size_t max_devices);
esp_err_t gw_c6_store_device_sync_endpoints(const gw_device_uid_t *uid);

esp_err_t gw_c6_store_endpoint_upsert(const gw_zb_endpoint_t *endpoint);
esp_err_t gw_c6_store_endpoint_remove_device(const gw_device_uid_t *uid);
size_t gw_c6_store_endpoint_list(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);
size_t gw_c6_store_endpoint_list_all(gw_zb_endpoint_t *out_eps, size_t max_eps);
bool gw_c6_store_find_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid);

#ifdef __cplusplus
}
#endif
