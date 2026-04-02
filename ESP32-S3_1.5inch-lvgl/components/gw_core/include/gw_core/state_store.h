#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_core/model_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_state_store_init(void);
esp_err_t gw_state_store_set_bool(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, bool value, uint64_t ts_ms);
esp_err_t gw_state_store_set_f32(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, float value, uint64_t ts_ms);
esp_err_t gw_state_store_set_u32(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, uint32_t value, uint64_t ts_ms);
esp_err_t gw_state_store_set_u64(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, uint64_t value, uint64_t ts_ms);
esp_err_t gw_state_store_set_text(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, const char *value, uint64_t ts_ms);
esp_err_t gw_state_store_get(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, gw_state_item_t *out);
esp_err_t gw_state_store_get_any(const gw_device_uid_t *uid, const char *key, gw_state_item_t *out);
size_t gw_state_store_list(const gw_device_uid_t *uid, uint8_t endpoint, gw_state_item_t *out, size_t max_out);
size_t gw_state_store_list_uid(const gw_device_uid_t *uid, gw_state_item_t *out, size_t max_out);
esp_err_t gw_state_store_remove_uid(const gw_device_uid_t *uid);
size_t gw_state_store_count(void);
esp_err_t gw_state_store_get_by_index(size_t index, gw_state_item_t *out);

#ifdef __cplusplus
}
#endif
