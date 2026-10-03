#include "web/web.h"

#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/*
 * Web service: Wi-Fi station поверх ESP-Hosted, затем HTTP-сервер с бинарным WS
 * (docs/services/WEB.md). Задача отдельная: подъём сети не задерживает bootstrap.
 *
 * Протокол пока — echo: проверяем транспорт. Кадры реального протокола — следующий шаг.
 */

static const char *TAG = "web";

#define WEB_TASK_STACK 4096
#define WEB_TASK_PRIORITY 4
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;

static const char *ROOT_PAGE =
    "<!doctype html><meta charset=utf-8><title>ESP32-P4 HA</title>"
    "<h1>ESP32-P4 Home Automation</h1><p>Web service is up.</p>";

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Подключение стартует после скана, а не здесь. */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* Диагностика и задел под UI: какие сети видит C6. */
static void wifi_log_scan(void)
{
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "set_ps(NONE) -> 0x%x", (unsigned)err);
    err = esp_wifi_set_country_code("RU", true);
    ESP_LOGI(TAG, "set_country(RU) -> 0x%x", (unsigned)err);

    wifi_scan_config_t scan = {0};
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = 120;
    scan.scan_time.active.max = 300;

    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_wifi_scan_start(&scan, true);
        uint16_t count = 0;
        const esp_err_t got = esp_wifi_scan_get_ap_num(&count);
        ESP_LOGI(TAG, "scan #%d: start=0x%x get_num=0x%x found=%u", attempt, (unsigned)err,
                 (unsigned)got, (unsigned)count);
        if (count == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        wifi_ap_record_t records[16] = {0};
        uint16_t shown = (count > 16) ? 16 : count;
        if (esp_wifi_scan_get_ap_records(&shown, records) != ESP_OK) {
            return;
        }
        for (uint16_t i = 0; i < shown; i++) {
            ESP_LOGI(TAG, "  AP \"%s\" ch=%u rssi=%d", (const char *)records[i].ssid,
                     (unsigned)records[i].primary, (int)records[i].rssi);
        }
        return;
    }
}

static sys_error_t wifi_connect(void)
{
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_NO_MEM);
    }

    /* netif/event loop могли быть подняты ESP-Hosted — повторная инициализация не нужна. */
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_IO);
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_WEB_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_WEB_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_start() != ESP_OK) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_IO);
    }

    wifi_log_scan();

    ESP_LOGI(TAG, "connecting to Wi-Fi SSID \"%s\"", CONFIG_WEB_WIFI_SSID);
    esp_wifi_connect();
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    return SYS_OK;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, ROOT_PAGE, HTTPD_RESP_USE_STRLEN);
}

/* Эхо бинарного кадра: проверка транспорта WS (протокол — следующий шаг). */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS client connected");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t buffer[256] = {0};
    if (frame.len > sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    frame.payload = buffer;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        return err;
    }

    frame.type = HTTPD_WS_TYPE_BINARY;
    return httpd_ws_send_frame(req, &frame);
}

static sys_error_t server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_IO);
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    const httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &ws);
    return SYS_OK;
}

static void web_task(void *arg)
{
    (void)arg;
    if (sys_failed(wifi_connect())) {
        ESP_LOGE(TAG, "Wi-Fi not connected; web server not started");
        vTaskDelete(NULL);
        return;
    }
    if (sys_failed(server_start())) {
        ESP_LOGE(TAG, "HTTP server not started");
    } else {
        ESP_LOGI(TAG, "web server started");
    }
    vTaskDelete(NULL);
}

sys_error_t web_start(domain_t *domain)
{
    (void)domain;
    if (xTaskCreate(web_task, "web", WEB_TASK_STACK, NULL, WEB_TASK_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
