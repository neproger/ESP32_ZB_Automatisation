#include "gw_http/gw_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_core/event_bus.h"

static const char *TAG = "gw_ws";

typedef struct {
    int fd;
    bool subscribed_events;
} gw_ws_client_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} cbor_wr_t;

static httpd_handle_t s_server;
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;

#define GW_WS_MAX_CLIENTS 4
#define GW_WS_EVENT_Q_CAP 8
#define GW_WS_EVENT_TASK_PRIO 2
#define GW_WS_EVENT_TASK_STACK 2560
static gw_ws_client_t s_clients[GW_WS_MAX_CLIENTS];
static QueueHandle_t s_event_q;
static TaskHandle_t s_event_task;
static bool s_event_q_caps_alloc;

static void ws_refresh_out_queue_binding(void)
{
    bool has_subscribers = false;
    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd != 0 && s_clients[i].subscribed_events) {
            has_subscribers = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);

    if (has_subscribers) {
        gw_event_bus_set_out_queue(s_event_q);
    } else {
        gw_event_bus_set_out_queue(NULL);
    }
}

static void ws_client_remove_fd(int fd)
{
    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i] = (gw_ws_client_t){0};
            break;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);
    ws_refresh_out_queue_binding();
}

static bool ws_client_add_fd(int fd)
{
    bool ok = false;
    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].subscribed_events = true;
            ok = true;
            break;
        }
    }
    if (!ok) {
        for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
            if (s_clients[i].fd == 0) {
                s_clients[i].fd = fd;
                s_clients[i].subscribed_events = true;
                ok = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_client_lock);
    if (ok) {
        ws_refresh_out_queue_binding();
    }
    return ok;
}

static bool cbor_wr_reserve(cbor_wr_t *w, size_t add)
{
    if (!w) return false;
    if (w->len + add <= w->cap) return true;
    size_t new_cap = w->cap ? w->cap : 128;
    while (new_cap < w->len + add) new_cap *= 2;
    uint8_t *nb = (uint8_t *)heap_caps_realloc(w->buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) {
        nb = (uint8_t *)heap_caps_realloc(w->buf, new_cap, MALLOC_CAP_8BIT);
    }
    if (!nb) return false;
    w->buf = nb;
    w->cap = new_cap;
    return true;
}

static bool cbor_wr_u8(cbor_wr_t *w, uint8_t v)
{
    if (!cbor_wr_reserve(w, 1)) return false;
    w->buf[w->len++] = v;
    return true;
}

static bool cbor_wr_mem(cbor_wr_t *w, const void *src, size_t n)
{
    if (!cbor_wr_reserve(w, n)) return false;
    memcpy(w->buf + w->len, src, n);
    w->len += n;
    return true;
}

static bool cbor_wr_uint(cbor_wr_t *w, uint8_t major, uint64_t v)
{
    if (v < 24) {
        return cbor_wr_u8(w, (uint8_t)((major << 5) | (uint8_t)v));
    }
    if (v <= 0xff) {
        if (!cbor_wr_u8(w, (uint8_t)((major << 5) | 24))) return false;
        return cbor_wr_u8(w, (uint8_t)v);
    }
    if (v <= 0xffff) {
        uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
        if (!cbor_wr_u8(w, (uint8_t)((major << 5) | 25))) return false;
        return cbor_wr_mem(w, b, sizeof(b));
    }
    if (v <= 0xffffffffULL) {
        uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        if (!cbor_wr_u8(w, (uint8_t)((major << 5) | 26))) return false;
        return cbor_wr_mem(w, b, sizeof(b));
    }
    uint8_t b[8] = {
        (uint8_t)(v >> 56), (uint8_t)(v >> 48), (uint8_t)(v >> 40), (uint8_t)(v >> 32),
        (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    if (!cbor_wr_u8(w, (uint8_t)((major << 5) | 27))) return false;
    return cbor_wr_mem(w, b, sizeof(b));
}

static bool cbor_wr_text(cbor_wr_t *w, const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s);
    if (!cbor_wr_uint(w, 3, n)) return false;
    return cbor_wr_mem(w, s, n);
}

static bool cbor_wr_bool(cbor_wr_t *w, bool v)
{
    return cbor_wr_u8(w, (uint8_t)((7 << 5) | (v ? 21 : 20)));
}

static bool cbor_wr_null(cbor_wr_t *w)
{
    return cbor_wr_u8(w, (uint8_t)((7 << 5) | 22));
}

static bool cbor_wr_i64(cbor_wr_t *w, int64_t v)
{
    if (v >= 0) {
        return cbor_wr_uint(w, 0, (uint64_t)v);
    }
    return cbor_wr_uint(w, 1, (uint64_t)(-1 - v));
}

static bool cbor_wr_f64(cbor_wr_t *w, double v)
{
    union {
        double d;
        uint64_t u;
    } u = {0};
    u.d = v;
    uint8_t b[8] = {
        (uint8_t)(u.u >> 56), (uint8_t)(u.u >> 48), (uint8_t)(u.u >> 40), (uint8_t)(u.u >> 32),
        (uint8_t)(u.u >> 24), (uint8_t)(u.u >> 16), (uint8_t)(u.u >> 8), (uint8_t)u.u};
    if (!cbor_wr_u8(w, (uint8_t)((7 << 5) | 27))) return false;
    return cbor_wr_mem(w, b, sizeof(b));
}

static void ws_transfer_done_cb(esp_err_t err, int socket, void *arg)
{
    (void)err;
    (void)socket;
    free(arg);
}

static esp_err_t ws_send_cbor_async(int fd, const uint8_t *buf, size_t len)
{
    if (!s_server || !buf || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        ws_client_remove_fd(fd);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buf, len);

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = copy,
        .len = len,
    };
    esp_err_t err = httpd_ws_send_data_async(s_server, fd, &frame, ws_transfer_done_cb, copy);
    if (err != ESP_OK) {
        free(copy);
        ws_client_remove_fd(fd);
        return err;
    }
    return ESP_OK;
}

static bool msg_kv_get(const char *msg, const char *key, char *out, size_t out_size)
{
    if (!msg || !key || !out || out_size == 0) return false;
    char needle[32];
    (void)snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(msg, needle);
    if (!p) return false;
    p += strlen(needle);
    size_t i = 0;
    while (p[i] && p[i] != ' ' && i + 1 < out_size) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static void map_state_key(uint16_t cluster, uint16_t attr, char *out, size_t out_size)
{
    if (cluster == 0x0006 && attr == 0x0000) {
        strlcpy(out, "onoff", out_size);
        return;
    }
    if (cluster == 0x0008 && attr == 0x0000) {
        strlcpy(out, "level", out_size);
        return;
    }
    if (cluster == 0x0300 && attr == 0x0003) {
        strlcpy(out, "color_x", out_size);
        return;
    }
    if (cluster == 0x0300 && attr == 0x0004) {
        strlcpy(out, "color_y", out_size);
        return;
    }
    if (cluster == 0x0300 && attr == 0x0007) {
        strlcpy(out, "color_temp_mireds", out_size);
        return;
    }
    if (cluster == 0x0402 && attr == 0x0000) {
        strlcpy(out, "temperature_c", out_size);
        return;
    }
    if (cluster == 0x0405 && attr == 0x0000) {
        strlcpy(out, "humidity_pct", out_size);
        return;
    }
    if (cluster == 0x0001 && attr == 0x0021) {
        strlcpy(out, "battery_pct", out_size);
        return;
    }
    if (cluster == 0x0001 && attr == 0x0020) {
        strlcpy(out, "battery_mv", out_size);
        return;
    }
    if (cluster == 0x0406 && attr == 0x0000) {
        strlcpy(out, "occupancy", out_size);
        return;
    }
    if (cluster == 0x0400 && attr == 0x0000) {
        strlcpy(out, "illuminance_raw", out_size);
        return;
    }
    if (cluster == 0x0403 && attr == 0x0000) {
        strlcpy(out, "pressure_raw", out_size);
        return;
    }
    (void)snprintf(out, out_size, "cluster_%04x_attr_%04x", (unsigned)cluster, (unsigned)attr);
}

static bool ws_encode_event(const gw_event_t *e, cbor_wr_t *w)
{
    if (!e || !w) return false;

    const char *out_type = NULL;
    enum {DATA_AUTOM_FIRED, DATA_AUTOM_RESULT, DATA_DEVICE_EVENT, DATA_DEVICE_STATE, DATA_GENERIC} data_kind;
    char automation_id[GW_AUTOMATION_ID_MAX] = {0};
    bool ok = false;
    bool has_ok = false;
    bool has_action_idx = false;
    uint32_t action_idx = 0;
    const char *err = NULL;
    char cmd[32] = {0};
    char state_key[40] = {0};
    const char *device_event_name = "command";

    if (strcmp(e->type, "rules.fired") == 0) {
        out_type = "automation.fired";
        data_kind = DATA_AUTOM_FIRED;
        if (!msg_kv_get(e->msg, "automation_id", automation_id, sizeof(automation_id))) return false;
    } else if (strcmp(e->type, "rules.action") == 0) {
        char tmp[16] = {0};
        out_type = "automation.result";
        data_kind = DATA_AUTOM_RESULT;
        if (!msg_kv_get(e->msg, "automation_id", automation_id, sizeof(automation_id))) return false;
        if (msg_kv_get(e->msg, "ok", tmp, sizeof(tmp))) {
            has_ok = true;
            ok = (strcmp(tmp, "1") == 0 || strcmp(tmp, "true") == 0);
        }
        if (msg_kv_get(e->msg, "idx", tmp, sizeof(tmp))) {
            has_action_idx = true;
            action_idx = (uint32_t)strtoul(tmp, NULL, 10);
        }
        const char *err_ptr = strstr(e->msg, "err=");
        if (err_ptr) {
            err_ptr += 4;
            if (*err_ptr) err = err_ptr;
        }
    } else if (strcmp(e->type, "zigbee.command") == 0) {
        out_type = "device.event";
        data_kind = DATA_DEVICE_EVENT;
        device_event_name = "command";
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_CMD) {
            strlcpy(cmd, e->payload_cmd, sizeof(cmd));
        }
    } else if (strcmp(e->type, "zigbee.attr_report") == 0 || strcmp(e->type, "zigbee.attr_read") == 0) {
        out_type = "device.state";
        data_kind = DATA_DEVICE_STATE;
        map_state_key(e->payload_cluster, e->payload_attr, state_key, sizeof(state_key));
    } else if (strcmp(e->type, "device.join") == 0 || strcmp(e->type, "zigbee.device_join") == 0) {
        out_type = "device.event";
        data_kind = DATA_DEVICE_EVENT;
        device_event_name = "join";
    } else if (strcmp(e->type, "device.leave") == 0 || strcmp(e->type, "zigbee.device_leave") == 0) {
        out_type = "device.event";
        data_kind = DATA_DEVICE_EVENT;
        device_event_name = "leave";
    } else if (strncmp(e->type, "zigbee.", 7) == 0 || strncmp(e->type, "zigbee_", 7) == 0 ||
               strncmp(e->type, "device.", 7) == 0 || strncmp(e->type, "automation.", 11) == 0) {
        out_type = "gateway.event";
        data_kind = DATA_GENERIC;
    } else {
        return false;
    }

    // envelope: { ts_ms, type, data }
    if (!cbor_wr_uint(w, 5, 3)) return false;
    if (!cbor_wr_text(w, "ts_ms") || !cbor_wr_uint(w, 0, e->ts_ms)) return false;
    if (!cbor_wr_text(w, "type") || !cbor_wr_text(w, out_type)) return false;
    if (!cbor_wr_text(w, "data")) return false;

    if (data_kind == DATA_AUTOM_FIRED) {
        if (!cbor_wr_uint(w, 5, 1)) return false;
        if (!cbor_wr_text(w, "automation_id") || !cbor_wr_text(w, automation_id)) return false;
        return true;
    }
    if (data_kind == DATA_AUTOM_RESULT) {
        uint64_t pairs = 2;
        if (has_action_idx) pairs++;
        if (err) pairs++;
        if (!cbor_wr_uint(w, 5, pairs)) return false;
        if (!cbor_wr_text(w, "automation_id") || !cbor_wr_text(w, automation_id)) return false;
        if (!cbor_wr_text(w, "ok") || !cbor_wr_bool(w, has_ok ? ok : false)) return false;
        if (has_action_idx) {
            if (!cbor_wr_text(w, "action_idx") || !cbor_wr_uint(w, 0, action_idx)) return false;
        }
        if (err) {
            if (!cbor_wr_text(w, "err") || !cbor_wr_text(w, err)) return false;
        }
        return true;
    }
    if (data_kind == DATA_DEVICE_EVENT) {
        uint64_t pairs = 3;
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) pairs++;
        if (cmd[0]) pairs++;
        if (!cbor_wr_uint(w, 5, pairs)) return false;
        if (!cbor_wr_text(w, "device_id") || !cbor_wr_text(w, e->device_uid)) return false;
        if (!cbor_wr_text(w, "event") || !cbor_wr_text(w, device_event_name)) return false;
        if (!cbor_wr_text(w, "source") || !cbor_wr_text(w, "zigbee")) return false;
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) {
            if (!cbor_wr_text(w, "endpoint_id") || !cbor_wr_uint(w, 0, e->payload_endpoint)) return false;
        }
        if (cmd[0]) {
            if (!cbor_wr_text(w, "cmd") || !cbor_wr_text(w, cmd)) return false;
        }
        return true;
    }
    if (data_kind == DATA_GENERIC) {
        uint64_t pairs = 4;
        if (e->device_uid[0] != '\0') pairs++;
        if (e->short_addr != 0) pairs++;
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) pairs++;
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) pairs++;
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR) pairs++;
        if (!cbor_wr_uint(w, 5, pairs)) return false;
        if (!cbor_wr_text(w, "event_type") || !cbor_wr_text(w, e->type)) return false;
        if (!cbor_wr_text(w, "source") || !cbor_wr_text(w, e->source)) return false;
        if (!cbor_wr_text(w, "msg") || !cbor_wr_text(w, e->msg)) return false;
        if (!cbor_wr_text(w, "has_value") || !cbor_wr_bool(w, (e->payload_flags & GW_EVENT_PAYLOAD_HAS_VALUE) != 0)) return false;
        if (e->device_uid[0] != '\0') {
            if (!cbor_wr_text(w, "device_id") || !cbor_wr_text(w, e->device_uid)) return false;
        }
        if (e->short_addr != 0) {
            if (!cbor_wr_text(w, "short_addr") || !cbor_wr_uint(w, 0, e->short_addr)) return false;
        }
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) {
            if (!cbor_wr_text(w, "endpoint_id") || !cbor_wr_uint(w, 0, e->payload_endpoint)) return false;
        }
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) {
            if (!cbor_wr_text(w, "cluster") || !cbor_wr_uint(w, 0, e->payload_cluster)) return false;
        }
        if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR) {
            if (!cbor_wr_text(w, "attr") || !cbor_wr_uint(w, 0, e->payload_attr)) return false;
        }
        return true;
    }

    if (!cbor_wr_uint(w, 5, 4)) return false;
    if (!cbor_wr_text(w, "device_id") || !cbor_wr_text(w, e->device_uid)) return false;
    if (!cbor_wr_text(w, "endpoint_id") || !cbor_wr_uint(w, 0, e->payload_endpoint)) return false;
    if (!cbor_wr_text(w, "key") || !cbor_wr_text(w, state_key)) return false;
    if (!cbor_wr_text(w, "value")) return false;
    if (!(e->payload_flags & GW_EVENT_PAYLOAD_HAS_VALUE)) {
        return cbor_wr_null(w);
    }
    switch ((gw_event_value_type_t)e->payload_value_type) {
        case GW_EVENT_VALUE_BOOL:
            return cbor_wr_bool(w, e->payload_value_bool != 0);
        case GW_EVENT_VALUE_I64:
            return cbor_wr_i64(w, e->payload_value_i64);
        case GW_EVENT_VALUE_F64:
            return cbor_wr_f64(w, e->payload_value_f64);
        case GW_EVENT_VALUE_TEXT:
            return cbor_wr_text(w, e->payload_value_text);
        default:
            return cbor_wr_null(w);
    }
}

static void ws_event_task_fn(void *arg)
{
    (void)arg;
    gw_event_t e;
    for (;;) {
        if (xQueueReceive(s_event_q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        gw_event_bus_record_event(&e);

        int fds[GW_WS_MAX_CLIENTS];
        size_t fd_count = 0;
        portENTER_CRITICAL(&s_client_lock);
        for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
            if (s_clients[i].fd != 0 && s_clients[i].subscribed_events) {
                fds[fd_count++] = s_clients[i].fd;
            }
        }
        portEXIT_CRITICAL(&s_client_lock);
        if (fd_count == 0) continue;

        cbor_wr_t w = {0};
        if (!ws_encode_event(&e, &w)) {
            free(w.buf);
            continue;
        }
        for (size_t i = 0; i < fd_count; i++) {
            esp_err_t err = ws_send_cbor_async(fds[i], w.buf, w.len);
            if (err == ESP_ERR_NO_MEM) {
                ESP_LOGW(TAG, "WS send OOM; dropping event");
                break;
            }
        }
        free(w.buf);
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        if (!ws_client_add_fd(fd)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "too many ws clients");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ws_client_remove_fd(fd);
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_client_remove_fd(fd);
        return ESP_OK;
    }
    if (frame.len > 0) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_8BIT);
        }
        if (!buf) return ESP_ERR_NO_MEM;
        frame.payload = buf;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        free(buf);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t gw_ws_register(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    if (s_server) return ESP_OK;

    s_server = server;
    memset(s_clients, 0, sizeof(s_clients));
    s_event_q_caps_alloc = false;
    s_event_q = xQueueCreateWithCaps(GW_WS_EVENT_Q_CAP, sizeof(gw_event_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_event_q) {
        s_event_q = xQueueCreate(GW_WS_EVENT_Q_CAP, sizeof(gw_event_t));
    } else {
        s_event_q_caps_alloc = true;
    }
    if (!s_event_q) {
        s_server = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(ws_event_task_fn, "ws_events", GW_WS_EVENT_TASK_STACK, NULL, GW_WS_EVENT_TASK_PRIO, &s_event_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (s_event_q_caps_alloc) {
            vQueueDeleteWithCaps(s_event_q);
        } else {
            vQueueDelete(s_event_q);
        }
        s_event_q = NULL;
        s_event_q_caps_alloc = false;
        s_server = NULL;
        return ESP_ERR_NO_MEM;
    }

    static const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = true,
#endif
    };
    esp_err_t err = httpd_register_uri_handler(s_server, &ws_uri);
    if (err != ESP_OK) {
        vTaskDelete(s_event_task);
        s_event_task = NULL;
        if (s_event_q_caps_alloc) {
            vQueueDeleteWithCaps(s_event_q);
        } else {
            vQueueDelete(s_event_q);
        }
        s_event_q = NULL;
        s_event_q_caps_alloc = false;
        s_server = NULL;
        return err;
    }
    gw_event_bus_set_out_queue(NULL);
    ESP_LOGI(TAG, "WebSocket enabled at /ws (CBOR)");
    return ESP_OK;
}

void gw_ws_unregister(void)
{
    if (!s_server) return;

    gw_event_bus_set_out_queue(NULL);
    portENTER_CRITICAL(&s_client_lock);
    memset(s_clients, 0, sizeof(s_clients));
    portEXIT_CRITICAL(&s_client_lock);

    if (s_event_task) {
        vTaskDelete(s_event_task);
        s_event_task = NULL;
    }
    if (s_event_q) {
        if (s_event_q_caps_alloc) {
            vQueueDeleteWithCaps(s_event_q);
        } else {
            vQueueDelete(s_event_q);
        }
        s_event_q = NULL;
        s_event_q_caps_alloc = false;
    }
    s_server = NULL;
}


