#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "gw_store/gw_store_schema.h"
#include "micro_db/micro_db_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_store_init_topology(void);
esp_err_t gw_store_deinit_topology(void);

esp_err_t gw_store_upsert_device(const gw_proto_device_v1_t *record,
                                 bool *out_changed,
                                 bool *out_inserted);
esp_err_t gw_store_get_device(const gw_device_uid_t *uid,
                              gw_proto_device_v1_t *out_record);
esp_err_t gw_store_remove_full_device(const gw_device_uid_t *uid,
                                      bool *out_removed);
esp_err_t gw_store_remove_device(const gw_device_uid_t *uid,
                                 bool *out_removed);
size_t gw_store_count_devices(void);
esp_err_t gw_store_get_device_by_index(size_t index,
                                       gw_proto_device_v1_t *out_record);
size_t gw_store_iter_devices(micro_db_iter_cb_t cb, void *user_ctx);

esp_err_t gw_store_upsert_endpoint(const gw_proto_endpoint_v1_t *record,
                                   bool *out_changed,
                                   bool *out_inserted);
esp_err_t gw_store_get_endpoint(const gw_store_endpoint_key_t *key,
                                gw_proto_endpoint_v1_t *out_record);
esp_err_t gw_store_remove_endpoint(const gw_store_endpoint_key_t *key,
                                   bool *out_removed);
size_t gw_store_count_endpoints(void);
esp_err_t gw_store_get_endpoint_by_index(size_t index,
                                         gw_proto_endpoint_v1_t *out_record);
size_t gw_store_iter_endpoints(micro_db_iter_cb_t cb, void *user_ctx);
size_t gw_store_iter_endpoints_for_device(const gw_device_uid_t *uid,
                                          micro_db_iter_cb_t cb,
                                          void *user_ctx);
bool gw_store_find_device_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid);

#ifdef __cplusplus
}
#endif
