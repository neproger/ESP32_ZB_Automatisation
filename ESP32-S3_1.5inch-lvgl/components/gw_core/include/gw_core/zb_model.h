#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_core/model_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_zb_model_init(void);
esp_err_t gw_zb_model_upsert_endpoint(const gw_zb_endpoint_t *ep);
esp_err_t gw_zb_model_remove_device(const gw_device_uid_t *uid);
size_t gw_zb_model_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);
esp_err_t gw_zb_model_get_endpoint_by_index(const gw_device_uid_t *uid, size_t index, gw_zb_endpoint_t *out_ep);
bool gw_zb_model_find_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid);

#ifdef __cplusplus
}
#endif
