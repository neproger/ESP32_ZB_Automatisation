#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_device_fb_store_init(void);
esp_err_t gw_device_fb_store_set(const uint8_t *buf, size_t len);
const uint8_t *gw_device_fb_store_get(size_t *out_len);
esp_err_t gw_device_fb_store_copy(uint8_t **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif
