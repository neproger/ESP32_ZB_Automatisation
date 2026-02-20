#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t screensaver_timeout_ms;
    uint32_t weather_success_interval_ms;
    uint32_t weather_retry_interval_ms;
    bool timezone_auto;
    int16_t timezone_offset_min;
} gw_project_settings_t;

esp_err_t gw_project_settings_init(void);
esp_err_t gw_project_settings_get(gw_project_settings_t *out);
esp_err_t gw_project_settings_set(const gw_project_settings_t *in);
void gw_project_settings_get_defaults(gw_project_settings_t *out);
bool gw_project_settings_validate(const gw_project_settings_t *in);

#ifdef __cplusplus
}
#endif
