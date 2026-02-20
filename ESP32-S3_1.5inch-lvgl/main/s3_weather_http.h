#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool valid;
    float temperature_c;
    float humidity_pct;
    float wind_speed_kmh;
    int weather_code;
    char observed_time[32];
} s3_weather_result_t;

esp_err_t s3_weather_http_fetch_once(double latitude,
                                     double longitude,
                                     int timeout_ms,
                                     s3_weather_result_t *out_result,
                                     char *out_error,
                                     size_t out_error_size);

#ifdef __cplusplus
}
#endif

