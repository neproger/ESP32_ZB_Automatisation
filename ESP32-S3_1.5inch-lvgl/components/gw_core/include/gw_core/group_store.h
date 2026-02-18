#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_group_store_init(void);

size_t gw_group_store_list(gw_group_entry_t *out, size_t max_out);
size_t gw_group_store_list_items(gw_group_item_t *out, size_t max_out);
size_t gw_group_store_list_items_for_group(const char *group_id, gw_group_item_t *out, size_t max_out);

esp_err_t gw_group_store_create(const char *id_opt, const char *name, gw_group_entry_t *out_created);
esp_err_t gw_group_store_rename(const char *id, const char *name);
esp_err_t gw_group_store_remove(const char *id);

esp_err_t gw_group_store_set_endpoint(const char *group_id, const gw_device_uid_t *device_uid, uint8_t endpoint);
esp_err_t gw_group_store_remove_endpoint(const gw_device_uid_t *device_uid, uint8_t endpoint);
esp_err_t gw_group_store_reorder_endpoint(const char *group_id, const gw_device_uid_t *device_uid, uint8_t endpoint, uint32_t order);
esp_err_t gw_group_store_set_endpoint_label(const gw_device_uid_t *device_uid, uint8_t endpoint, const char *label);

#ifdef __cplusplus
}
#endif
