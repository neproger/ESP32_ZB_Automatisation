#include "web/web_model_file.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "web/web_projection.h"
#include "web/web_proto.h"

/*
 * MODEL-блоб (статическая часть canonical-модели: devices/entities/capabilities/
 * automations/groups/location/settings) собирается в PSRAM целиком, без фиксированного
 * слота: при переполнении буфер удваивается. Отдаётся по GET /model.bin, пересобирается
 * лениво — при первом запросе после изменения MODEL-типа.
 */

static const char *TAG = "web_model";

#define MODEL_BUF_INITIAL (8u * 1024u)

static uint8_t *s_buf;
static size_t s_cap;
static size_t s_len; /* 0 — ещё ни разу не собран */
static volatile bool s_dirty = true;
static SemaphoreHandle_t s_lock;

void web_model_file_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void web_model_file_mark_dirty(void)
{
    s_dirty = true;
}

/* Собрать блоб в s_buf. Вызывается под s_lock. false — не удалось выделить память. */
static bool model_build(void)
{
    if (s_buf == NULL) {
        s_buf = heap_caps_malloc_prefer(MODEL_BUF_INITIAL, 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_buf == NULL) {
            return false;
        }
        s_cap = MODEL_BUF_INITIAL;
    }

    for (;;) {
        uint32_t count = 0;
        const size_t payload = web_projection_build_model(
            s_buf + WEB_SNAPSHOT_HDR_SIZE, s_cap - WEB_SNAPSHOT_HDR_SIZE, &count);
        if (payload != 0 || count == 0) {
            s_buf[0] = 'H';
            s_buf[1] = 'A';
            s_buf[2] = 'S';
            s_buf[3] = 'M';
            s_buf[4] = (uint8_t)WEB_SNAPSHOT_VER;
            s_buf[5] = 0;
            s_buf[6] = 0;
            s_buf[7] = 0;
            s_buf[8] = (uint8_t)(count & 0xffu);
            s_buf[9] = (uint8_t)((count >> 8) & 0xffu);
            s_buf[10] = (uint8_t)((count >> 16) & 0xffu);
            s_buf[11] = (uint8_t)((count >> 24) & 0xffu);
            s_len = WEB_SNAPSHOT_HDR_SIZE + payload;
            s_dirty = false;
            return true;
        }

        /* Переполнение — модель больше текущего буфера: удваиваем. */
        const size_t next = s_cap * 2;
        uint8_t *grown = heap_caps_malloc_prefer(next, 2,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (grown == NULL) {
            return false;
        }
        heap_caps_free(s_buf);
        s_buf = grown;
        s_cap = next;
    }
}

esp_err_t web_model_file_serve(httpd_req_t *req)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    if ((s_dirty || s_len == 0) && !model_build()) {
        ESP_LOGE(TAG, "model not built: out of memory");
        result = httpd_resp_send_500(req);
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
        /* Имена не версионируются: запрещаем кэш, иначе браузер держит старую модель. */
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        result = httpd_resp_send(req, (const char *)s_buf, (ssize_t)s_len);
    }

    xSemaphoreGive(s_lock);
    return result;
}
