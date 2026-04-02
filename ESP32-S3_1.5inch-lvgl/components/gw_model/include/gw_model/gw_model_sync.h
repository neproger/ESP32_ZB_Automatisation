#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_sync_init(void);
esp_err_t gw_model_sync_deinit(void);

#ifdef __cplusplus
}
#endif
