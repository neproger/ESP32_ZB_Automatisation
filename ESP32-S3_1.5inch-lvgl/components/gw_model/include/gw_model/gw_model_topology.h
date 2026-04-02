#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "gw_model/gw_model_schema.h"
#include "micro_db/micro_db_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_init_topology(void);
esp_err_t gw_model_deinit_topology(void);

esp_err_t gw_model_upsert_device(const gw_proto_device_v1_t *record,
                                 bool *out_changed,
                                 bool *out_inserted);
esp_err_t gw_model_get_device(const gw_device_uid_t *uid,
                              gw_proto_device_v1_t *out_record);
esp_err_t gw_model_remove_device(const gw_device_uid_t *uid,
                                 bool *out_removed);
size_t gw_model_count_devices(void);
esp_err_t gw_model_get_device_by_index(size_t index,
                                       gw_proto_device_v1_t *out_record);
size_t gw_model_iter_devices(micro_db_iter_cb_t cb, void *user_ctx);

esp_err_t gw_model_upsert_endpoint(const gw_proto_endpoint_v1_t *record,
                                   bool *out_changed,
                                   bool *out_inserted);
esp_err_t gw_model_get_endpoint(const gw_model_endpoint_key_t *key,
                                gw_proto_endpoint_v1_t *out_record);
esp_err_t gw_model_remove_endpoint(const gw_model_endpoint_key_t *key,
                                   bool *out_removed);
size_t gw_model_count_endpoints(void);
esp_err_t gw_model_get_endpoint_by_index(size_t index,
                                         gw_proto_endpoint_v1_t *out_record);
size_t gw_model_iter_endpoints(micro_db_iter_cb_t cb, void *user_ctx);
size_t gw_model_iter_endpoints_for_device(const gw_device_uid_t *uid,
                                          micro_db_iter_cb_t cb,
                                          void *user_ctx);
bool gw_model_find_device_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid);

#ifdef __cplusplus
}
#endif
