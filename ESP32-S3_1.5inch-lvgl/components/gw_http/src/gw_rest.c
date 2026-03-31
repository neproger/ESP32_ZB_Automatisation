#include "gw_http/gw_rest.h"

esp_err_t gw_http_register_rest_endpoints(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
