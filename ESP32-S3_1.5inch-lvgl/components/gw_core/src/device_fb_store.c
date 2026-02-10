#include "gw_core/device_fb_store.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static uint8_t *s_buf;
static size_t s_len;
static bool s_inited;

esp_err_t gw_device_fb_store_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t gw_device_fb_store_set(const uint8_t *buf, size_t len)
{
    if (!s_inited || !s_lock || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buf, len);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t *old = s_buf;
    s_buf = copy;
    s_len = len;
    xSemaphoreGive(s_lock);
    free(old);
    return ESP_OK;
}

const uint8_t *gw_device_fb_store_get(size_t *out_len)
{
    if (!s_inited || !s_lock) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint8_t *ptr = s_buf;
    size_t len = s_len;
    xSemaphoreGive(s_lock);

    if (out_len) {
        *out_len = len;
    }
    return ptr;
}

esp_err_t gw_device_fb_store_copy(uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len || !s_inited || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_buf || s_len == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *copy = (uint8_t *)malloc(s_len);
    if (!copy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_buf, s_len);
    size_t len = s_len;
    xSemaphoreGive(s_lock);

    *out_buf = copy;
    *out_len = len;
    return ESP_OK;
}
