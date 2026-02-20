#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool valid;
    double latitude;
    double longitude;
    int32_t utc_offset_sec;
    char city[40];
    char region[40];
    char timezone[48];
} s3_geoip_result_t;

esp_err_t s3_geoip_http_fetch_once(int timeout_ms, s3_geoip_result_t *out_result, char *out_error, size_t out_error_size);

#ifdef __cplusplus
}
#endif
