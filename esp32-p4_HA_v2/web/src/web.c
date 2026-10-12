#include "web/web.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "web/web_commands.h"
#include "web/web_model_file.h"
#include "web/web_projection.h"
#include "web/web_proto.h"
#include "web/web_sessions.h"

/*
 * Web — HTTP/WebSocket-сервер canonical-модели. Роль сервиса: принять соединение,
 * раздать snapshot, доставлять дельты и принимать команды. Вся предметная работа —
 * в модулях: web_sessions (доставка), web_projection (canonical → браузер),
 * web_commands (браузер → Domain/Device), web_proto (framing).
 */

static const char *TAG = "web";

#define WEB_TASK_STACK 6144
#define WEB_TASK_PRIORITY 4
#define WEB_INBOX_LENGTH 64
#define WEB_CMD_BUF WEB_PROTO_MAX_FRAME

static domain_t *s_domain;
static httpd_handle_t s_server;
static QueueHandle_t s_inbox;

/*
 * Элемент inbox: факт Domain (kind 0) либо запрос снапшота задачи web (kind 1 — полный,
 * kind 2 — STATE). open/ack идут синхронно в httpd-задаче (транспорт под мьютексом).
 */
typedef struct {
    uint8_t kind; /* 0 — факт, 1 — snapshot(full), 2 — snapshot_state */
    int fd;
    domain_event_t event;
} web_inbox_item_t;

static void web_request_snapshot(int fd)
{
    const web_inbox_item_t item = {.kind = 1, .fd = fd};
    xQueueSend(s_inbox, &item, 0);
}

static void web_request_state(int fd)
{
    const web_inbox_item_t item = {.kind = 2, .fd = fd};
    xQueueSend(s_inbox, &item, 0);
}

/* --- HTTP / WebSocket --------------------------------------------------- */

#ifdef WEB_UI_EMBEDDED
/* Собранный web-ui встроен в прошивку (web/ui). index.html — как есть, скрипты/стили — gzip. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[] asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[] asm("_binary_app_css_gz_end");

static esp_err_t send_embedded(httpd_req_t *req, const char *content_type, const uint8_t *begin,
                               const uint8_t *end, bool gzip)
{
    httpd_resp_set_type(req, content_type);
    /* Имена файлов фиксированные: запрещаем кэш, иначе браузер держит старую сборку. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (gzip) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    return httpd_resp_send(req, (const char *)begin, (ssize_t)(end - begin));
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_embedded(req, "text/html; charset=utf-8", index_html_start, index_html_end, false);
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    return send_embedded(req, "application/javascript; charset=utf-8", app_js_gz_start,
                         app_js_gz_end, true);
}

static esp_err_t app_css_handler(httpd_req_t *req)
{
    return send_embedded(req, "text/css; charset=utf-8", app_css_gz_start, app_css_gz_end, true);
}
#else
static const char *ROOT_PAGE =
    "<!doctype html><meta charset=utf-8><title>ESP32-P4 HA</title>"
    "<h1>ESP32-P4 Home Automation</h1><p>Web service is up (no embedded UI).</p>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, ROOT_PAGE, HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        web_sessions_open(fd); /* регистрация синхронно (транспорт под мьютексом) */
        ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);
        return ESP_OK;
    }

    uint8_t buf[WEB_CMD_BUF];
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.len > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        return err;
    }

    web_hdr_t hdr;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (frame.len >= WEB_PROTO_HDR_SIZE &&
        web_frame_decode(frame.payload, frame.len, &hdr, &payload, &payload_len)) {
        if (hdr.type == (uint8_t)WEB_MSG_ACK) {
            web_sessions_ack(fd, hdr.seq);
        } else if (hdr.type == (uint8_t)WEB_MSG_COMMAND && payload_len >= 1) {
            if (payload[0] == (uint8_t)WEB_CMD_SNAPSHOT) {
                web_request_snapshot(fd); /* полный snapshot (fallback) */
            } else if (payload[0] == (uint8_t)WEB_CMD_SNAPSHOT_STATE) {
                web_request_state(fd); /* только состояния */
            } else {
                web_commands_handle(payload[0], payload + 1, payload_len - 1);
            }
        }
    }
    return ESP_OK;
}

static sys_error_t server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Верхняя граница задана LWIP: 3 сокета держит сам HTTP-сервер (дефолт 7). */
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_IO);
    }
    web_sessions_init(s_server);

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
    httpd_register_uri_handler(s_server, &root);
    const httpd_uri_t model = {
        .uri = "/model.bin",
        .method = HTTP_GET,
        .handler = web_model_file_serve,
    };
    httpd_register_uri_handler(s_server, &model);
#ifdef WEB_UI_EMBEDDED
    const httpd_uri_t app_js = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = app_js_handler,
    };
    const httpd_uri_t app_css = {
        .uri = "/app.css",
        .method = HTTP_GET,
        .handler = app_css_handler,
    };
    httpd_register_uri_handler(s_server, &app_js);
    httpd_register_uri_handler(s_server, &app_css);
#endif
    httpd_register_uri_handler(s_server, &ws);
    return SYS_OK;
}

/* --- подписка Domain ---------------------------------------------------- */

static bool web_accept(const domain_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_inbox == NULL) {
        return false;
    }
    const web_inbox_item_t item = {.kind = 0, .event = *event};
    return xQueueSend(s_inbox, &item, 0) == pdTRUE;
}

static sys_error_t web_subscribe(void)
{
    s_inbox = xQueueCreate(WEB_INBOX_LENGTH, sizeof(web_inbox_item_t));
    if (s_inbox == NULL) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_NO_MEM);
    }

    domain_subscription_desc_t desc = {0};
    desc.kind_mask = (1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED) |
                     (1u << (uint32_t)DOMAIN_FACT_ENTITY_REMOVED) |
                     (1u << (uint32_t)DOMAIN_FACT_EVENT);
    desc.source_mask = 0; /* любые источники */
    desc.entity = 0;      /* любые типы */
    desc.try_push = web_accept;
    desc.ctx = NULL;

    domain_subscription_t *sub = NULL;
    return domain_subscribe(s_domain, &desc, &sub);
}

/* --- задача ------------------------------------------------------------- */

static void web_task(void *arg)
{
    s_domain = (domain_t *)arg;
    web_projection_init(s_domain);
    web_commands_init(s_domain);
    web_model_file_init();

    if (sys_failed(server_start())) {
        ESP_LOGE(TAG, "HTTP server not started");
        vTaskDelete(NULL);
        return;
    }
    if (sys_failed(web_subscribe())) {
        ESP_LOGE(TAG, "domain subscribe failed; deltas disabled");
    }
    ESP_LOGI(TAG, "web server started");

    for (;;) {
        /* Дренаж очереди: все запросы к транспорту обрабатываем в этой задаче. */
        web_inbox_item_t item = {0};
        bool got = false;
        while (xQueueReceive(s_inbox, &item, 0) == pdTRUE) {
            got = true;
            switch (item.kind) {
            case 0:
                web_projection_on_event(&item.event);
                if ((item.event.kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED ||
                     item.event.kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) &&
                    web_projection_is_model(item.event.entity)) {
                    web_model_file_mark_dirty();
                }
                break;
            case 1:
                web_projection_snapshot(item.fd);
                break;
            case 2:
                web_projection_send_state(item.fd);
                break;
            default:
                break;
            }
        }
        web_sessions_step();
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

sys_error_t web_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_INVALID_ARG);
    }
    if (xTaskCreate(web_task, "web", WEB_TASK_STACK, domain, WEB_TASK_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
