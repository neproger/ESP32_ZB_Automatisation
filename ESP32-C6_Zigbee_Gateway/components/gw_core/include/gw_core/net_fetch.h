#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int timeout_ms;
    int max_body_bytes;
} gw_net_fetch_cfg_t;

esp_err_t gw_net_fetch_get_text(const char *url, const gw_net_fetch_cfg_t *cfg, char *out, size_t out_size, int *out_http_status);
esp_err_t gw_net_fetch_get_json_number(const char *url, const gw_net_fetch_cfg_t *cfg, const char *json_path, double *out_value);
esp_err_t gw_net_fetch_get_json_text(const char *url, const gw_net_fetch_cfg_t *cfg, const char *json_path, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
