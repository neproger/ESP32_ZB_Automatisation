#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t s3_weather_service_start(void);
void s3_weather_service_get_location(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
