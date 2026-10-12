#include "web/web_model_file.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web/web_projection.h"
#include "web/web_proto.h"

/*
 * Раздел "model": meta (1 сектор) + два слота (двойной буфер). Пересборка пишет в
 * НЕАКТИВНЫЙ слот, затем переключает meta — активный слот всё время цел. Запись/чтение
 * не под Domain-lock (собираем в RAM).
 *
 * Пересборка — в ОТДЕЛЬНОЙ задаче: flash erase/write не должен стопорить WS-отправку в
 * web-задаче. Producer (web-задача) только помечает dirty и будит задачу (notify).
 *
 * meta (20 байт): magic 'W','M','M','T' | rev u32 | active u8 | reserved[3] |
 *                 slot_len u32 | crc32 u32.
 */

static const char *TAG = "web_model";

#define MODEL_MAGIC 0x544d4d57u /* 'W','M','M','T' LE */
#define META_OFF 0x0u
#define META_SIZE 0x1000u
#define SLOT_A_OFF META_SIZE
#define SLOT_B_OFF (META_SIZE + 0x4000u)
#define SLOT_SIZE 0x4000u
#define MODEL_REBUILD_STACK 4096
#define MODEL_REBUILD_PRIO 2
#define MODEL_REBUILD_DEBOUNCE_MS 500u

static const esp_partition_t *s_part;
static TaskHandle_t s_task;
static bool s_ready;

static void meta_pack(uint8_t m[20], uint32_t rev, uint8_t active, uint32_t len, uint32_t crc)
{
    memset(m, 0, 20);
    m[0] = (uint8_t)(MODEL_MAGIC & 0xffu);
    m[1] = (uint8_t)((MODEL_MAGIC >> 8) & 0xffu);
    m[2] = (uint8_t)((MODEL_MAGIC >> 16) & 0xffu);
    m[3] = (uint8_t)((MODEL_MAGIC >> 24) & 0xffu);
    m[4] = (uint8_t)(rev & 0xffu);
    m[5] = (uint8_t)((rev >> 8) & 0xffu);
    m[6] = (uint8_t)((rev >> 16) & 0xffu);
    m[7] = (uint8_t)((rev >> 24) & 0xffu);
    m[8] = active;
    m[12] = (uint8_t)(len & 0xffu);
    m[13] = (uint8_t)((len >> 8) & 0xffu);
    m[14] = (uint8_t)((len >> 16) & 0xffu);
    m[15] = (uint8_t)((len >> 24) & 0xffu);
    m[16] = (uint8_t)(crc & 0xffu);
    m[17] = (uint8_t)((crc >> 8) & 0xffu);
    m[18] = (uint8_t)((crc >> 16) & 0xffu);
    m[19] = (uint8_t)((crc >> 24) & 0xffu);
}

static bool meta_read(uint32_t *rev, uint8_t *active, uint32_t *len)
{
    uint8_t m[20];
    if (s_part == NULL || esp_partition_read(s_part, META_OFF, m, sizeof(m)) != ESP_OK) {
        return false;
    }
    const uint32_t magic = (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) |
                           ((uint32_t)m[3] << 24);
    if (magic != MODEL_MAGIC || m[8] > 1) {
        return false;
    }
    *rev = (uint32_t)m[4] | ((uint32_t)m[5] << 8) | ((uint32_t)m[6] << 16) |
           ((uint32_t)m[7] << 24);
    *active = m[8];
    *len = (uint32_t)m[12] | ((uint32_t)m[13] << 8) | ((uint32_t)m[14] << 16) |
           ((uint32_t)m[15] << 24);
    return true;
}

/* Собрать и записать блоб в неактивный слот, затем переключить meta. */
static esp_err_t model_rebuild(void)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc_prefer(SLOT_SIZE, 2,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t count = 0;
    const size_t payload = web_projection_build_model(buf + WEB_SNAPSHOT_HDR_SIZE,
                                                      SLOT_SIZE - WEB_SNAPSHOT_HDR_SIZE, &count);
    if (payload == 0 && count != 0) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    const size_t len = WEB_SNAPSHOT_HDR_SIZE + payload;
    buf[0] = 'H';
    buf[1] = 'A';
    buf[2] = 'S';
    buf[3] = 'M';
    buf[4] = (uint8_t)WEB_SNAPSHOT_VER;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    buf[8] = (uint8_t)(count & 0xffu);
    buf[9] = (uint8_t)((count >> 8) & 0xffu);
    buf[10] = (uint8_t)((count >> 16) & 0xffu);
    buf[11] = (uint8_t)((count >> 24) & 0xffu);

    const uint32_t crc = esp_rom_crc32_le(0, buf, (uint32_t)len);

    uint32_t prev_rev = 0;
    uint8_t prev_active = 0;
    uint32_t prev_len = 0;
    (void)meta_read(&prev_rev, &prev_active, &prev_len);
    const uint8_t target = (prev_active == 0) ? 1 : 0;
    const uint32_t target_off = (target == 0) ? SLOT_A_OFF : SLOT_B_OFF;

    esp_err_t err = esp_partition_erase_range(s_part, target_off, SLOT_SIZE);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, target_off, buf, len);
    }
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write model failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t m[20];
    meta_pack(m, prev_rev + 1, target, (uint32_t)len, crc);
    err = esp_partition_erase_range(s_part, META_OFF, META_SIZE);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, META_OFF, m, sizeof(m));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write meta failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "model.bin rebuilt: rev=%u slot=%u len=%u count=%u",
             (unsigned)(prev_rev + 1), (unsigned)target, (unsigned)len, (unsigned)count);
    return ESP_OK;
}

static void model_rebuild_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(MODEL_REBUILD_DEBOUNCE_MS));
        (void)ulTaskNotifyTake(pdTRUE, 0); /* схлопнуть серию изменений */
        if (model_rebuild() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000)); /* повторить позже */
            xTaskNotifyGive(s_task);
        }
    }
}

void web_model_file_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      "model");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "partition 'model' not found; /model.bin disabled");
        return;
    }
    s_ready = true;
    if (xTaskCreate(model_rebuild_task, "model_file", MODEL_REBUILD_STACK, NULL,
                    MODEL_REBUILD_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return;
    }
    uint32_t rev = 0;
    uint8_t active = 0;
    uint32_t len = 0;
    if (!meta_read(&rev, &active, &len) || len == 0) {
        web_model_file_mark_dirty(); /* первый запуск — собрать */
    }
}

void web_model_file_mark_dirty(void)
{
    if (s_ready && s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t web_model_file_serve(httpd_req_t *req)
{
    if (s_part == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    uint32_t rev = 0;
    uint8_t active = 0;
    uint32_t len = 0;
    if (!meta_read(&rev, &active, &len) || len == 0) {
        httpd_resp_send_500(req); /* ещё не собрано (фон) → клиент уйдёт в fallback */
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const uint32_t base = (active == 0) ? SLOT_A_OFF : SLOT_B_OFF;
    uint8_t chunk[1024];
    uint32_t off = 0;
    while (off < len) {
        const uint32_t n = (len - off > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : (len - off);
        if (esp_partition_read(s_part, base + off, chunk, n) != ESP_OK) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, (const char *)chunk, (ssize_t)n) != ESP_OK) {
            return ESP_FAIL;
        }
        off += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
