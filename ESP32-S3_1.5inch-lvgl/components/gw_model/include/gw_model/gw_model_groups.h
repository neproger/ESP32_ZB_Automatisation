#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "gw_model/gw_model_schema.h"
#include "micro_db/micro_db_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_init_groups(void);
esp_err_t gw_model_deinit_groups(void);

esp_err_t gw_model_upsert_group(const gw_proto_group_v1_t *record,
                                bool *out_changed,
                                bool *out_inserted);
esp_err_t gw_model_get_group(const char *group_id,
                             gw_proto_group_v1_t *out_record);
esp_err_t gw_model_remove_group(const char *group_id,
                                bool *out_removed);
size_t gw_model_count_groups(void);
esp_err_t gw_model_get_group_by_index(size_t index,
                                      gw_proto_group_v1_t *out_record);
size_t gw_model_iter_groups(micro_db_iter_cb_t cb, void *user_ctx);

esp_err_t gw_model_upsert_group_item(const gw_proto_group_item_v1_t *record,
                                     bool *out_changed,
                                     bool *out_inserted);
esp_err_t gw_model_get_group_item(const gw_model_endpoint_key_t *key,
                                  gw_proto_group_item_v1_t *out_record);
esp_err_t gw_model_remove_group_item(const gw_model_endpoint_key_t *key,
                                     bool *out_removed);
size_t gw_model_count_group_items(void);
esp_err_t gw_model_get_group_item_by_index(size_t index,
                                           gw_proto_group_item_v1_t *out_record);
size_t gw_model_iter_group_items(micro_db_iter_cb_t cb, void *user_ctx);

size_t gw_model_count_group_items_for_group(const char *group_id);
esp_err_t gw_model_get_group_item_for_group_by_index(const char *group_id,
                                                     size_t index,
                                                     gw_proto_group_item_v1_t *out_record);
size_t gw_model_iter_group_items_for_group(const char *group_id,
                                           micro_db_iter_cb_t cb,
                                           void *user_ctx);

esp_err_t gw_model_create_group(const char *id_opt,
                                const char *name,
                                gw_proto_group_v1_t *out_created);
esp_err_t gw_model_rename_group(const char *group_id,
                                const char *name);
esp_err_t gw_model_set_group_item(const char *group_id,
                                  const gw_device_uid_t *device_uid,
                                  uint8_t endpoint);
esp_err_t gw_model_remove_group_item_by_endpoint(const gw_device_uid_t *device_uid,
                                                 uint8_t endpoint);
esp_err_t gw_model_reorder_group_item(const char *group_id,
                                      const gw_device_uid_t *device_uid,
                                      uint8_t endpoint,
                                      uint32_t order);
esp_err_t gw_model_set_group_item_label(const gw_device_uid_t *device_uid,
                                        uint8_t endpoint,
                                        const char *label);

#ifdef __cplusplus
}
#endif
