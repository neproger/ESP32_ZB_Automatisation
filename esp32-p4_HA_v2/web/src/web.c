#include "web/web.h"

#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha_model/ha_automation.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "web/web_proto.h"

/*
 * Web service (docs/services/WEB.md, docs/services/WEB_PROTOCOL.md).
 *
 * Wi-Fi — station поверх ESP-Hosted (external C3). Поверх HTTP поднят бинарный WS;
 * на проводе — сырые записи Domain как есть (вариант A): никаких DTO/строк.
 *
 * Задача web:
 *   - поднимает Wi-Fi и HTTP+WS (два шага в одной задаче);
 *   - на подключение клиента шлёт snapshot (SYNC_BEGIN → ENTITY… → SYNC_END);
 *   - подписана на Domain и рассылает дельты (ENTITY / ENTITY_REMOVE) всем клиентам;
 *   - выполняет команды из WS (Zigbee-команда, CRUD автоматизаций, переименование).
 */

static const char *TAG = "web";

#define WEB_TASK_STACK 6144
#define WEB_TASK_PRIORITY 4
#define WEB_MAX_CLIENTS 8
#define WEB_INBOX_LENGTH 32
#define WEB_CMD_BUF 128

_Static_assert(WEB_ENTITY_DEVICE == (uint32_t)HA_ENTITY_DEVICE, "entity id: device");
_Static_assert(WEB_ENTITY_STATE == (uint32_t)HA_ENTITY_STATE, "entity id: state");
_Static_assert(WEB_ENTITY_ENDPOINT == (uint32_t)HA_ENTITY_ENDPOINT, "entity id: endpoint");
_Static_assert(WEB_ENTITY_AUTOMATION == (uint32_t)HA_ENTITY_AUTOMATION, "entity id: automation");

/* Фиксируем layout провода: эти размеры зеркалит web-ui/src/schema.js. */
_Static_assert(sizeof(ha_zb_command_t) == 32, "zb command layout: update web-ui");
_Static_assert(sizeof(ha_automation_record_t) == 48, "automation record layout: update web-ui");

/* Схема записи: браузер знает те же размеры (web-ui/src/schema.js). */
typedef struct {
    uint8_t type;
    uint16_t key_size;
    uint16_t rec_size;
} web_schema_t;

static const web_schema_t SCHEMA[] = {
    {WEB_ENTITY_DEVICE, sizeof(ha_device_uid_t), sizeof(ha_device_record_t)},
    {WEB_ENTITY_STATE, sizeof(ha_zb_state_key_t), sizeof(ha_zb_state_record_t)},
    {WEB_ENTITY_ENDPOINT, sizeof(ha_endpoint_key_t), sizeof(ha_endpoint_record_t)},
    {WEB_ENTITY_AUTOMATION, sizeof(ha_automation_key_t), sizeof(ha_automation_record_t)},
};
#define WEB_SCHEMA_COUNT (sizeof(SCHEMA) / sizeof(SCHEMA[0]))

/* Элемент inbox задачи: факт от Domain либо запрос snapshot конкретному fd. */
typedef struct {
    uint8_t kind; /* 0 — факт, 1 — snapshot */
    int fd;
    domain_event_t event;
} web_inbox_item_t;

static domain_t *s_domain;
static httpd_handle_t s_server;
static QueueHandle_t s_inbox;

static const web_schema_t *schema_for(uint8_t type)
{
    for (size_t i = 0; i < WEB_SCHEMA_COUNT; i++) {
        if (SCHEMA[i].type == type) {
            return &SCHEMA[i];
        }
    }
    return NULL;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void web_send_frame(int fd, uint8_t type, uint16_t seq, const void *payload, uint16_t len)
{
    uint8_t frame[WEB_PROTO_MAX_FRAME];
    const size_t n = web_frame_encode(frame, sizeof(frame), type, seq, payload, len);
    if (n == 0) {
        return;
    }
    httpd_ws_frame_t ws = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = frame,
        .len = n,
    };
    /* Несмотря на имя, шлёт синхронно: payload нужен только на время вызова. */
    httpd_ws_send_frame_async(s_server, fd, &ws);
}

static void web_broadcast(uint8_t type, uint16_t seq, const void *payload, uint16_t len)
{
    int fds[WEB_MAX_CLIENTS];
    size_t count = WEB_MAX_CLIENTS;
    if (httpd_get_client_list(s_server, &count, fds) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            web_send_frame(fds[i], type, seq, payload, len);
        }
    }
}

/* --- snapshot ----------------------------------------------------------- */

static bool snapshot_count(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    (*(uint32_t *)ctx)++;
    return true;
}

typedef struct {
    int fd;
    const web_schema_t *schema;
    uint32_t *count;
} snapshot_ctx_t;

static bool snapshot_emit(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t payload[1 + DOMAIN_EVENT_KEY_MAX + sizeof(ha_endpoint_record_t)];
    payload[0] = s->schema->type;
    memcpy(payload + 1, key, s->schema->key_size);
    memcpy(payload + 1 + s->schema->key_size, record, s->schema->rec_size);
    web_send_frame(s->fd, WEB_MSG_ENTITY, 0, payload,
                   (uint16_t)(1 + s->schema->key_size + s->schema->rec_size));
    (*s->count)++;
    return true;
}

static void web_send_snapshot(int fd)
{
    uint32_t total = 0;
    for (size_t i = 0; i < WEB_SCHEMA_COUNT; i++) {
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, snapshot_count, &total);
    }

    uint8_t word[4];
    put_u32(word, total);
    web_send_frame(fd, WEB_MSG_SYNC_BEGIN, 0, word, sizeof(word));

    uint32_t sent = 0;
    for (size_t i = 0; i < WEB_SCHEMA_COUNT; i++) {
        snapshot_ctx_t ctx = {.fd = fd, .schema = &SCHEMA[i], .count = &sent};
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, snapshot_emit, &ctx);
    }

    put_u32(word, sent);
    web_send_frame(fd, WEB_MSG_SYNC_END, 0, word, sizeof(word));
}

/* --- delta -------------------------------------------------------------- */

static void web_send_fact(const domain_event_t *event)
{
    const web_schema_t *schema = schema_for((uint8_t)event->entity);
    if (schema == NULL || event->key_size != schema->key_size) {
        return;
    }

    uint8_t payload[1 + DOMAIN_EVENT_KEY_MAX + sizeof(ha_endpoint_record_t)];
    payload[0] = schema->type;
    memcpy(payload + 1, event->key, schema->key_size);

    if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
        uint8_t record[sizeof(ha_endpoint_record_t)];
        if (sys_failed(domain_entity_get(s_domain, event->entity, event->key, record))) {
            return;
        }
        memcpy(payload + 1 + schema->key_size, record, schema->rec_size);
        web_broadcast(WEB_MSG_ENTITY, 0, payload,
                      (uint16_t)(1 + schema->key_size + schema->rec_size));
    } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
        web_broadcast(WEB_MSG_ENTITY_REMOVE, 0, payload, (uint16_t)(1 + schema->key_size));
    }
}

/* --- команды ------------------------------------------------------------ */

static void web_reply(int fd, uint16_t seq, uint16_t code)
{
    const uint8_t payload[2] = {(uint8_t)(code & 0xffu), (uint8_t)(code >> 8)};
    web_send_frame(fd, WEB_MSG_CMD_RESULT, seq, payload, sizeof(payload));
}

static uint16_t web_do_zb_command(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_zb_command_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_zb_command_t command;
    memcpy(&command, args, sizeof(command));

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    meta.value.type = (uint8_t)DOMAIN_VALUE_ENUM;
    meta.value.v.u32 = command.command_id;

    const sys_error_t err =
        domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_device_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_uid_t) + HA_DEVICE_NAME_MAX) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_uid_t uid;
    memcpy(&uid, args, sizeof(uid));

    ha_device_record_t record = {0};
    sys_error_t err = domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &record);
    if (sys_failed(err)) {
        return err.code;
    }
    memcpy(record.name, args + sizeof(uid), HA_DEVICE_NAME_MAX);
    record.name[HA_DEVICE_NAME_MAX - 1] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &record, &meta,
                            &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_automation_put(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_automation_key_t) + sizeof(ha_automation_record_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_automation_key_t key;
    ha_automation_record_t record;
    memcpy(&key, args, sizeof(key));
    memcpy(&record, args + sizeof(key), sizeof(record));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, &key,
                                              &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_automation_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_automation_key_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_automation_key_t key;
    memcpy(&key, args, sizeof(key));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, &key, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static void web_request_snapshot(int fd)
{
    const web_inbox_item_t item = {.kind = 1, .fd = fd};
    xQueueSend(s_inbox, &item, 0);
}

static void web_handle_command(int fd, uint16_t seq, uint8_t cmd, const uint8_t *args, size_t len)
{
    if (cmd == (uint8_t)WEB_CMD_SNAPSHOT) {
        web_request_snapshot(fd);
        return; /* ответа нет: клиент получит SYNC_BEGIN… */
    }

    uint16_t status = (uint16_t)SYS_CODE_INVALID_ARG;
    switch (cmd) {
    case WEB_CMD_ZB_COMMAND:
        status = web_do_zb_command(args, len);
        break;
    case WEB_CMD_DEVICE_RENAME:
        status = web_do_device_rename(args, len);
        break;
    case WEB_CMD_AUTOMATION_PUT:
        status = web_do_automation_put(args, len);
        break;
    case WEB_CMD_AUTOMATION_REMOVE:
        status = web_do_automation_remove(args, len);
        break;
    default:
        break;
    }
    web_reply(fd, seq, status);
}

/* --- Wi-Fi -------------------------------------------------------------- */

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* Диагностика: что видит радио C3 при подъёме. */
static void wifi_log_scan(void)
{
    ESP_LOGI(TAG, "set_ps(NONE) -> 0x%x", (unsigned)esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "set_country(RU) -> 0x%x", (unsigned)esp_wifi_set_country_code("RU", true));

    wifi_scan_config_t scan = {0};
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = 120;
    scan.scan_time.active.max = 300;

    for (int attempt = 1; attempt <= 3; attempt++) {
        const esp_err_t err = esp_wifi_scan_start(&scan, true);
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

/* --- HTTP / WebSocket --------------------------------------------------- */

#ifdef WEB_UI_EMBEDDED
/* Собранный web-ui встроен в прошивку (web/ui, символы EMBED_FILES). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");

static esp_err_t send_embedded(httpd_req_t *req, const char *content_type, const uint8_t *begin,
                               const uint8_t *end)
{
    httpd_resp_set_type(req, content_type);
    return httpd_resp_send(req, (const char *)begin, (ssize_t)(end - begin));
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_embedded(req, "text/html; charset=utf-8", index_html_start, index_html_end);
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    return send_embedded(req, "application/javascript; charset=utf-8", app_js_start, app_js_end);
}

static esp_err_t app_css_handler(httpd_req_t *req)
{
    return send_embedded(req, "text/css; charset=utf-8", app_css_start, app_css_end);
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
        /* Snapshot запрашивает клиент командой SNAPSHOT: слать из GET нельзя —
         * ответ 101 завершается после возврата хендлера, и кадр уйдёт раньше. */
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
        web_frame_decode(frame.payload, frame.len, &hdr, &payload, &payload_len) &&
        hdr.type == (uint8_t)WEB_MSG_COMMAND && payload_len >= 1) {
        web_handle_command(fd, hdr.seq, payload[0], payload + 1, payload_len - 1);
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
    desc.kind_mask =
        (1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED) | (1u << (uint32_t)DOMAIN_FACT_ENTITY_REMOVED);
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

    if (sys_failed(wifi_connect())) {
        ESP_LOGE(TAG, "Wi-Fi not connected; web server not started");
        vTaskDelete(NULL);
        return;
    }
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
        web_inbox_item_t item = {0};
        if (xQueueReceive(s_inbox, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.kind == 0) {
            web_send_fact(&item.event);
        } else {
            web_send_snapshot(item.fd);
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
