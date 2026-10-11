#include "web/web.h"

#include <string.h>

#include "device/device_service.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha_model/ha_automation.h"
#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_logical_device.h"
#include "ha_model/ha_rule.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
#include "web/web_proto.h"

/*
 * Web — транспорт canonical-модели. Он НЕ интерпретирует Domain: кадры несут
 * canonical structs verbatim.
 *
 *   UPSERT: [type=ha_entity_t][key][record]   (record — canonical struct как есть)
 *   REMOVE: [type][key]
 *   EVENT:  [WEB_MSG_EVENT][ha_event_t]
 *   COMMAND: [cmd][args] (args — canonical struct verbatim)
 *
 * Роль web: HTTP/WS-сервер, сессии, TX-очередь (seq/ACK/retry), framing, snapshot и
 * дельта-рассылка generic-обходом Domain, декодирование команд в Domain-операции.
 * Никаких компактных DTO: canonical layout = wire layout (LE, fixed-width, reserved=0).
 */

static const char *TAG = "web";

#define WEB_TASK_STACK 6144
#define WEB_TASK_PRIORITY 4
#define WEB_MAX_CLIENTS 8
#define WEB_INBOX_LENGTH 32
#define WEB_CMD_BUF WEB_PROTO_MAX_FRAME

static domain_t *s_domain;
static httpd_handle_t s_server;
static QueueHandle_t s_inbox;

/* Элемент inbox: факт Domain либо запрос snapshot конкретному fd. */
typedef struct {
    uint8_t kind; /* 0 — факт, 1 — snapshot */
    int fd;
    domain_event_t event;
} web_inbox_item_t;

/* --- транспорт: per-client TX-очередь + stop-and-wait (seq + ACK) --------- */

#define WEB_TX_MAX_FRAMES 64
#define WEB_TX_MAX_BYTES (16u * 1024u)
#define WEB_TX_ACK_TIMEOUT_MS 1500u
#define WEB_TX_MAX_RETRY 3u

typedef struct web_tx_node {
    struct web_tx_node *next;
    uint16_t len;
    uint16_t seq;
    uint8_t data[WEB_PROTO_MAX_FRAME];
} web_tx_node_t;

typedef struct {
    bool used;
    int fd;
    web_tx_node_t *head;
    web_tx_node_t *tail;
    size_t frames;
    size_t bytes;
    uint16_t next_seq;
    bool waiting;
    uint16_t waiting_seq;
    uint8_t retry;
    TickType_t sent_at;
} web_client_t;

static web_client_t s_clients[WEB_MAX_CLIENTS];

static web_client_t *client_find(int fd)
{
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static void client_clear(web_client_t *c)
{
    web_tx_node_t *n = c->head;
    while (n != NULL) {
        web_tx_node_t *next = n->next;
        vPortFree(n);
        n = next;
    }
    const int fd = c->fd;
    memset(c, 0, sizeof(*c));
    c->fd = fd;
}

/* Слишком медленный/мёртвый клиент: закрыть сессию, очередь отбросить. */
static void client_kill(web_client_t *c)
{
    const int fd = c->fd;
    client_clear(c);
    c->used = false;
    (void)httpd_sess_trigger_close(s_server, fd);
}

static web_client_t *client_open(int fd)
{
    web_client_t *c = client_find(fd);
    if (c == NULL) {
        for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
            if (!s_clients[i].used) {
                c = &s_clients[i];
                break;
            }
        }
    }
    if (c == NULL) {
        return NULL;
    }
    client_clear(c);
    c->used = true;
    c->fd = fd;
    return c;
}

static void web_tx_ack(int fd, uint16_t seq)
{
    web_client_t *c = client_find(fd);
    if (c == NULL || !c->waiting || c->waiting_seq != seq) {
        return;
    }
    web_tx_node_t *n = c->head;
    if (n != NULL) {
        c->head = n->next;
        if (c->head == NULL) {
            c->tail = NULL;
        }
        c->frames--;
        c->bytes -= n->len;
        vPortFree(n);
    }
    c->waiting = false;
    c->retry = 0;
}

static void web_tx_enqueue(int fd, const uint8_t *frame, uint16_t len)
{
    if (len < WEB_PROTO_HDR_SIZE) {
        return;
    }
    web_client_t *c = client_find(fd);
    if (c == NULL) {
        c = client_open(fd);
        if (c == NULL) {
            return;
        }
    }
    if (c->frames >= WEB_TX_MAX_FRAMES || c->bytes + len > WEB_TX_MAX_BYTES) {
        client_kill(c); /* клиент не успевает: сессию закрываем, reconnect → snapshot */
        return;
    }
    web_tx_node_t *n = (web_tx_node_t *)pvPortMalloc(sizeof(web_tx_node_t));
    if (n == NULL) {
        client_kill(c);
        return;
    }
    n->next = NULL;
    n->len = len;
    n->seq = c->next_seq++;
    memcpy(n->data, frame, len);
    n->data[4] = (uint8_t)(n->seq & 0xffu); /* header.seq = транспортом присвоенный */
    n->data[5] = (uint8_t)(n->seq >> 8);
    if (c->tail != NULL) {
        c->tail->next = n;
    } else {
        c->head = n;
    }
    c->tail = n;
    c->frames++;
    c->bytes += len;
}

static bool client_alive(const web_client_t *c)
{
    return httpd_ws_get_fd_info(s_server, c->fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static void web_tx_send_head(web_client_t *c)
{
    httpd_ws_frame_t ws = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = c->head->data,
        .len = c->head->len,
    };
    if (httpd_ws_send_frame_async(s_server, c->fd, &ws) != ESP_OK) {
        client_kill(c);
        return;
    }
    c->waiting = true;
    c->waiting_seq = c->head->seq;
    c->sent_at = xTaskGetTickCount();
}

static void web_tx_step(void)
{
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        web_client_t *c = &s_clients[i];
        if (!c->used) {
            continue;
        }
        if (!client_alive(c)) {
            client_clear(c);
            c->used = false;
            continue;
        }
        if (c->head == NULL) {
            c->waiting = false;
            continue;
        }
        if (!c->waiting) {
            c->retry = 0;
            web_tx_send_head(c);
            continue;
        }
        const uint32_t elapsed_ms =
            (uint32_t)(xTaskGetTickCount() - c->sent_at) * portTICK_PERIOD_MS;
        if (elapsed_ms < WEB_TX_ACK_TIMEOUT_MS) {
            continue;
        }
        if (c->retry >= WEB_TX_MAX_RETRY) {
            client_kill(c); /* ACK не пришёл после ретраев: клиент мёртв */
            continue;
        }
        c->retry++;
        web_tx_send_head(c);
    }
}

static void web_send_frame(int fd, uint8_t type, const void *payload, uint16_t len)
{
    uint8_t frame[WEB_PROTO_MAX_FRAME];
    const size_t n = web_frame_encode(frame, sizeof(frame), type, 0, payload, len);
    if (n == 0) {
        return;
    }
    web_tx_enqueue(fd, frame, (uint16_t)n);
}

static void web_broadcast(uint8_t type, const void *payload, uint16_t len)
{
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].used) {
            web_send_frame(s_clients[i].fd, type, payload, len);
        }
    }
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

/* --- canonical wire: type = ha_entity_t, key/record verbatim --------------- */

/*
 * Типы, отдаваемые frontend. Список явный: новая внутренняя Domain-таблица не
 * становится публичным API автоматически. Key/record — canonical struct sizes.
 */
typedef struct {
    ha_entity_t type;
    uint16_t key_size;
    uint16_t rec_size;
} web_schema_t;

static const web_schema_t SCHEMA[] = {
    {HA_ENTITY_LOGICAL_DEVICE, sizeof(ha_logical_device_key_t),
     sizeof(ha_logical_device_record_t)},
    {HA_ENTITY_ENTITY, sizeof(ha_entity_key_t), sizeof(ha_entity_record_t)},
    {HA_ENTITY_PROPERTY_STATE, sizeof(ha_state_key_t), sizeof(ha_state_record_t)},
    {HA_ENTITY_CAPABILITIES, sizeof(ha_entity_key_t), sizeof(ha_entity_capabilities_record_t)},
    {HA_ENTITY_AUTOMATION, sizeof(ha_automation_key_t), sizeof(ha_rule_t)},
    {HA_ENTITY_GROUP_ITEM, sizeof(ha_group_item_key_t), sizeof(ha_group_item_record_t)},
    {HA_ENTITY_GROUP, sizeof(ha_group_key_t), sizeof(ha_group_record_t)},
    {HA_ENTITY_LOCATION, sizeof(ha_device_id_t), sizeof(ha_location_record_t)},
    {HA_ENTITY_WEATHER, sizeof(ha_device_id_t), sizeof(ha_weather_record_t)},
    {HA_ENTITY_WIFI_STATUS, sizeof(ha_device_id_t), sizeof(ha_wifi_status_record_t)},
    {HA_ENTITY_SETTINGS, sizeof(ha_settings_key_t), sizeof(ha_settings_record_t)},
};
#define SCHEMA_COUNT (sizeof(SCHEMA) / sizeof(SCHEMA[0]))

static const web_schema_t *schema_for(uint32_t type)
{
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        if ((uint32_t)SCHEMA[i].type == type) {
            return &SCHEMA[i];
        }
    }
    return NULL;
}

/* --- snapshot ----------------------------------------------------------- */

static bool scan_count(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    (*(uint32_t *)ctx)++;
    return true;
}

typedef struct {
    int fd;
    uint8_t type;
    uint16_t key_size;
    uint16_t rec_size;
    uint32_t sent;
} snapshot_ctx_t;

static bool scan_emit(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    memcpy(out, key, s->key_size);
    memcpy(out + s->key_size, record, s->rec_size);
    web_send_frame(s->fd, s->type, out, (uint16_t)(s->key_size + s->rec_size));
    s->sent++;
    return true;
}

static void web_send_snapshot(int fd)
{
    uint32_t total = 0;
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, scan_count, &total);
    }

    uint8_t word[4];
    put_u32(word, total);
    web_send_frame(fd, WEB_MSG_SYNC_BEGIN, word, sizeof(word));

    uint32_t sent = 0;
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        snapshot_ctx_t ctx = {.fd = fd,
                              .type = (uint8_t)SCHEMA[i].type,
                              .key_size = SCHEMA[i].key_size,
                              .rec_size = SCHEMA[i].rec_size,
                              .sent = 0};
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, scan_emit, &ctx);
        sent += ctx.sent;
    }

    put_u32(word, sent);
    web_send_frame(fd, WEB_MSG_SYNC_END, word, sizeof(word));
}

/* --- delta -------------------------------------------------------------- */

/* EVENT (канонический факт): payload = ha_event_t verbatim. */
static void web_send_event(const domain_event_t *event)
{
    if (event->entity != (uint32_t)HA_ENTITY_ENTITY || event->payload_ref == 0 ||
        event->payload_size != sizeof(ha_event_t)) {
        return;
    }
    ha_event_t ev = {0};
    if (sys_failed(domain_payload_get(s_domain, event->payload_ref, &ev, sizeof(ev)))) {
        return; /* payload вытеснен — событие не несём (редкое, best-effort) */
    }
    web_broadcast(WEB_MSG_EVENT, &ev, sizeof(ev));
}

/* UPSERT/REMOVE по canonical типу: [type][key][record] / [type][key]. */
static void web_send_fact(const domain_event_t *event)
{
    if (event->kind == (uint8_t)DOMAIN_FACT_EVENT) {
        web_send_event(event);
        return;
    }

    const web_schema_t *s = schema_for(event->entity);
    if (s == NULL || event->key_size != s->key_size) {
        return;
    }

    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
        if (sys_failed(domain_entity_get(s_domain, event->entity, event->key,
                                         out + s->key_size))) {
            return;
        }
        memcpy(out, event->key, s->key_size);
        web_broadcast((uint8_t)s->type, out, (uint16_t)(s->key_size + s->rec_size));
    } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
        web_broadcast((uint8_t)s->type, event->key, s->key_size);
    }
}

/* --- команды: args = canonical struct verbatim → Domain ------------------ */

static uint16_t web_do_property_command(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_property_command_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_post(s_domain, HA_CMD_PROPERTY, args, len, NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_entity_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_entity_rename_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_post(s_domain, HA_CMD_ENTITY_RENAME, args, len, NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Rename Logical Device: ha_device_id_t + char name[32] (verbatim, через Device service). */
static uint16_t web_do_device_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_id_t) + HA_LOGICAL_DEVICE_NAME_MAX) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_id_t device_id = 0;
    memcpy(&device_id, args, sizeof(device_id));
    char name[HA_LOGICAL_DEVICE_NAME_MAX + 1] = {0};
    memcpy(name, args + sizeof(device_id), HA_LOGICAL_DEVICE_NAME_MAX);
    const sys_error_t err = device_rename(s_domain, (uint8_t)DOMAIN_SOURCE_UI, device_id, name);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Remove Logical Device: ha_device_id_t (adapter remove + cascade через Device service). */
static uint16_t web_do_logical_device_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_id_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_id_t device_id = 0;
    memcpy(&device_id, args, sizeof(device_id));
    const sys_error_t err = device_remove(s_domain, (uint8_t)DOMAIN_SOURCE_UI, device_id);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t put_entity(const uint8_t *args, size_t len, ha_entity_t type, uint16_t key_size,
                           uint16_t rec_size)
{
    if (len != (size_t)key_size + rec_size) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err =
        domain_entity_put(s_domain, (domain_entity_t)type, args, args + key_size, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t remove_entity(const uint8_t *args, size_t len, ha_entity_t type,
                              uint16_t key_size)
{
    if (len != key_size) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_entity_remove(s_domain, (domain_entity_t)type, args, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_automation_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_AUTOMATION, sizeof(ha_automation_key_t),
                      sizeof(ha_rule_t));
}

static uint16_t web_do_automation_remove(const uint8_t *args, size_t len)
{
    return remove_entity(args, len, HA_ENTITY_AUTOMATION, sizeof(ha_automation_key_t));
}

static uint16_t web_do_group_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_GROUP, sizeof(ha_group_key_t),
                      sizeof(ha_group_record_t));
}

static uint16_t web_do_group_remove(const uint8_t *args, size_t len)
{
    return remove_entity(args, len, HA_ENTITY_GROUP, sizeof(ha_group_key_t));
}

static uint16_t web_do_group_item_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_GROUP_ITEM, sizeof(ha_group_item_key_t),
                      sizeof(ha_group_item_record_t));
}

static uint16_t web_do_group_item_remove(const uint8_t *args, size_t len)
{
    return remove_entity(args, len, HA_ENTITY_GROUP_ITEM, sizeof(ha_group_item_key_t));
}

static uint16_t web_do_location_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_LOCATION, sizeof(ha_device_id_t),
                      sizeof(ha_location_record_t));
}

static uint16_t web_do_discovery(const uint8_t *args, size_t len)
{
    if (len != 1) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_post(s_domain, HA_CMD_DISCOVERY, args, len, NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static void web_request_snapshot(int fd)
{
    const web_inbox_item_t item = {.kind = 1, .fd = fd};
    xQueueSend(s_inbox, &item, 0);
}

static void web_handle_command(int fd, uint8_t cmd, const uint8_t *args, size_t len)
{
    if (cmd == (uint8_t)WEB_CMD_SNAPSHOT) {
        web_request_snapshot(fd);
        return; /* клиент получит SYNC_BEGIN… */
    }

    uint16_t status = (uint16_t)SYS_CODE_INVALID_ARG;
    switch (cmd) {
    case WEB_CMD_ENTITY_RENAME:
        status = web_do_entity_rename(args, len);
        break;
    case WEB_CMD_DEVICE_RENAME:
        status = web_do_device_rename(args, len);
        break;
    case WEB_CMD_LOGICAL_DEVICE_REMOVE:
        status = web_do_logical_device_remove(args, len);
        break;
    case WEB_CMD_PROPERTY_COMMAND:
        status = web_do_property_command(args, len);
        break;
    case WEB_CMD_AUTOMATION_PUT:
        status = web_do_automation_put(args, len);
        break;
    case WEB_CMD_AUTOMATION_REMOVE:
        status = web_do_automation_remove(args, len);
        break;
    case WEB_CMD_DISCOVERY:
        status = web_do_discovery(args, len);
        break;
    case WEB_CMD_GROUP_PUT:
        status = web_do_group_put(args, len);
        break;
    case WEB_CMD_GROUP_REMOVE:
        status = web_do_group_remove(args, len);
        break;
    case WEB_CMD_GROUP_ITEM_PUT:
        status = web_do_group_item_put(args, len);
        break;
    case WEB_CMD_GROUP_ITEM_REMOVE:
        status = web_do_group_item_remove(args, len);
        break;
    case WEB_CMD_LOCATION_PUT:
        status = web_do_location_put(args, len);
        break;
    default:
        break;
    }
    if (status != (uint16_t)SYS_CODE_OK) {
        ESP_LOGW(TAG, "command %u rejected: code=%u", (unsigned)cmd, (unsigned)status);
    }
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
        /* Snapshot запрашивает клиент командой SNAPSHOT: слать из GET нельзя —
         * ответ 101 завершается после возврата хендлера, и кадр уйдёт раньше. */
        (void)client_open(fd);
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
            web_tx_ack(fd, hdr.seq);
        } else if (hdr.type == (uint8_t)WEB_MSG_COMMAND && payload_len >= 1) {
            if (client_find(fd) == NULL) {
                (void)client_open(fd);
            }
            web_handle_command(fd, payload[0], payload + 1, payload_len - 1);
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
        if (xQueueReceive(s_inbox, &item, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (item.kind == 0) {
                web_send_fact(&item.event);
            } else {
                web_send_snapshot(item.fd);
            }
        }
        web_tx_step();
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
