#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double latitude;
    double longitude;
    uint32_t refresh_interval_ms;
    uint32_t request_timeout_ms;
    bool refresh_on_init;
} gw_weather_cfg_t;

typedef struct {
    bool valid;
    float temperature_c;
    float humidity_pct;
    float wind_speed_kmh;
    int weather_code;
    uint64_t updated_mono_ms;
    char observed_time[32];
} gw_weather_snapshot_t;

esp_err_t gw_weather_init(const gw_weather_cfg_t *cfg);
esp_err_t gw_weather_deinit(void);

bool gw_weather_is_ready(void);
bool gw_weather_bootstrap_done(void);
esp_err_t gw_weather_request_refresh(void);
esp_err_t gw_weather_get_snapshot(gw_weather_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif
