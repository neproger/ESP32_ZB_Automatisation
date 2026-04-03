#pragma once

#include "esp_err.h"

#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_init_settings(void);
esp_err_t gw_model_deinit_settings(void);

esp_err_t gw_model_set_settings(const gw_proto_settings_v1_t *record,
                                bool *out_changed,
                                bool *out_inserted);
esp_err_t gw_model_get_settings(gw_proto_settings_v1_t *out_record);
bool gw_model_settings_validate(const gw_proto_settings_v1_t *record);

#ifdef __cplusplus
}
#endif
