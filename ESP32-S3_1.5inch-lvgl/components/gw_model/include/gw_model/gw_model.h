#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_init(void);
esp_err_t gw_model_deinit(void);

#ifdef __cplusplus
}
#endif
