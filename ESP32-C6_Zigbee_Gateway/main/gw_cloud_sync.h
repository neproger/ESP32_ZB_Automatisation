#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_cloud_sync_start(void);
esp_err_t gw_cloud_sync_set_wifi_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
