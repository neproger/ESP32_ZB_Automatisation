#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t s3_http_client_get_text(const char *url,
                                  int timeout_ms,
                                  size_t max_body_size,
                                  char **out_body,
                                  int *out_http_status,
                                  char *out_error,
                                  size_t out_error_size);

#ifdef __cplusplus
}
#endif
