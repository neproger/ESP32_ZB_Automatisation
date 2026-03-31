#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Internal: register/unregister websocket endpoint(s) on the given server.
esp_err_t gw_ws_register(httpd_handle_t server);
void gw_ws_unregister(void);
esp_err_t gw_ws_suppress_device_updates(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

