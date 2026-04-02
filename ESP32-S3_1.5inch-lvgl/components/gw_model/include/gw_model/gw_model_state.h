#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "gw_model/gw_model_schema.h"
#include "micro_db/micro_db_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_init_state(void);
esp_err_t gw_model_deinit_state(void);

esp_err_t gw_model_upsert_state(const gw_proto_state_item_v1_t *record,
                                bool *out_changed,
                                bool *out_inserted);
esp_err_t gw_model_get_state(const gw_model_state_key_t *key,
                             gw_proto_state_item_v1_t *out_record);
esp_err_t gw_model_remove_state(const gw_model_state_key_t *key,
                                bool *out_removed);
size_t gw_model_count_state(void);
esp_err_t gw_model_get_state_by_index(size_t index,
                                      gw_proto_state_item_v1_t *out_record);
size_t gw_model_iter_state(micro_db_iter_cb_t cb, void *user_ctx);
size_t gw_model_iter_state_for_endpoint(const gw_model_endpoint_key_t *owner,
                                        micro_db_iter_cb_t cb,
                                        void *user_ctx);
size_t gw_model_iter_state_for_device(const gw_device_uid_t *uid,
                                      micro_db_iter_cb_t cb,
                                      void *user_ctx);

#ifdef __cplusplus
}
#endif
