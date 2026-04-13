#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gw_model/gw_model_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_init_device_meta(void);
esp_err_t gw_model_deinit_device_meta(void);

esp_err_t gw_model_set_device_name(const gw_device_uid_t *uid,
                                   const char *name,
                                   bool *out_changed);
esp_err_t gw_model_get_device_meta(const gw_device_uid_t *uid,
                                   gw_model_device_meta_t *out_meta);
esp_err_t gw_model_remove_device_meta(const gw_device_uid_t *uid,
                                      bool *out_removed);
void gw_model_apply_device_meta(gw_proto_device_v1_t *device);

#ifdef __cplusplus
}
#endif
