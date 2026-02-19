#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_wifi_sta_start(const char *ssid, const char *password);
bool gw_wifi_sta_is_connected(void);

#ifdef __cplusplus
}
#endif
