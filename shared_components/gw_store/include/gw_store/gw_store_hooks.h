#pragma once

#include "esp_err.h"

#include "gw_proto/gw_proto.h"
#include "gw_store/gw_store_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

void gw_store_hook_remove_state_for_endpoint(const gw_store_endpoint_key_t *endpoint_key);

esp_err_t gw_store_hook_notify_device_upsert(const gw_proto_device_v1_t *record);
esp_err_t gw_store_hook_notify_device_remove(const gw_device_uid_t *uid);
esp_err_t gw_store_hook_notify_endpoint_upsert(const gw_proto_endpoint_v1_t *record);
esp_err_t gw_store_hook_notify_endpoint_remove(const gw_store_endpoint_key_t *key, uint16_t short_addr);

#ifdef __cplusplus
}
#endif
