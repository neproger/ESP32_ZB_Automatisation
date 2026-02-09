#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_http_register_rest_endpoints(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
