#include "gw_core/event_bus.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

ESP_EVENT_DEFINE_BASE(GW_EVENT_BASE);

static const char *TAG = "gw_event";

static bool s_inited;

// Event id generator. Ring buffer disabled to save RAM.
static uint32_t s_next_id = 1;
static portMUX_TYPE s_id_lock = portMUX_INITIALIZER_UNLOCKED;

// Optional listeners called on publish.
#define GW_EVENT_LISTENER_CAP 4
typedef struct {
    gw_event_bus_listener_t cb;
    void *user_ctx;
} gw_event_listener_slot_t;
static gw_event_listener_slot_t s_listeners[GW_EVENT_LISTENER_CAP];
static portMUX_TYPE s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t s_out_q;

static bool event_should_go_to_out_queue(const char *type)
{
    if (!type || !type[0]) {
        return false;
    }
    if (strcmp(type, "rules.fired") == 0 || strcmp(type, "rules.action") == 0) {
        return true;
    }
    if (strcmp(type, "device.join") == 0 || strcmp(type, "device.leave") == 0) {
        return true;
    }
    if (strncmp(type, "zigbee.", 7) == 0 || strncmp(type, "zigbee_", 7) == 0) {
        return true;
    }
    return false;
}

static void safe_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

void gw_event_bus_publish_ex(const char *type,
                            const char *source,
                            const char *device_uid,
                            uint16_t short_addr,
                            const char *msg,
                            const uint8_t *payload_cbor,
                            size_t payload_len);

static void gw_event_bus_publish_internal(const char *type,
                                          const char *source,
                                          const char *device_uid,
                                          uint16_t short_addr,
                                          const char *msg,
                                          uint8_t payload_flags,
                                          uint8_t endpoint,
                                          const char *cmd,
                                          uint16_t cluster_id,
                                          uint16_t attr_id,
                                          gw_event_value_type_t value_type,
                                          bool value_bool,
                                          int64_t value_i64,
                                          double value_f64,
                                          const char *value_text,
                                          const uint8_t *payload_cbor,
                                          size_t payload_len);

esp_err_t gw_event_bus_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    // Uses the default event loop created by esp_event_loop_create_default()
    portENTER_CRITICAL(&s_id_lock);
    s_next_id = 1;
    portEXIT_CRITICAL(&s_id_lock);

    portENTER_CRITICAL(&s_listener_lock);
    memset(s_listeners, 0, sizeof(s_listeners));
    portEXIT_CRITICAL(&s_listener_lock);

    s_inited = true;
    return ESP_OK;
}

esp_err_t gw_event_bus_post(gw_event_id_t id, const void *data, size_t data_size, TickType_t ticks_to_wait)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_post(GW_EVENT_BASE, (int32_t)id, data, data_size, ticks_to_wait);
}

uint32_t gw_event_bus_last_id(void)
{
    uint32_t last = 0;
    portENTER_CRITICAL(&s_id_lock);
    last = (s_next_id > 0) ? (s_next_id - 1) : 0;
    portEXIT_CRITICAL(&s_id_lock);
    return last;
}

void gw_event_bus_publish(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const char *msg)
{
    gw_event_bus_publish_internal(type, source, device_uid, short_addr, msg, 0, 0, NULL, 0, 0,
                                  GW_EVENT_VALUE_NONE, false, 0, 0.0, NULL, NULL, 0);
}

void gw_event_bus_publish_cbor(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const uint8_t *payload_cbor, size_t payload_len)
{
    gw_event_bus_publish_internal(type, source, device_uid, short_addr, "", 0, 0, NULL, 0, 0,
                                  GW_EVENT_VALUE_NONE, false, 0, 0.0, NULL, payload_cbor, payload_len);
}

void gw_event_bus_publish_zb(const char *type,
                             const char *source,
                             const char *device_uid,
                             uint16_t short_addr,
                             const char *msg,
                             uint8_t endpoint,
                             const char *cmd,
                             uint16_t cluster_id,
                             uint16_t attr_id,
                             gw_event_value_type_t value_type,
                             bool value_bool,
                             int64_t value_i64,
                             double value_f64,
                             const char *value_text,
                             const uint8_t *payload_cbor,
                             size_t payload_len)
{
    uint8_t flags = 0;
    if (endpoint > 0) flags |= GW_EVENT_PAYLOAD_HAS_ENDPOINT;
    if (cmd && cmd[0]) flags |= GW_EVENT_PAYLOAD_HAS_CMD;
    if (cluster_id) flags |= GW_EVENT_PAYLOAD_HAS_CLUSTER;
    if (attr_id) flags |= GW_EVENT_PAYLOAD_HAS_ATTR;
    if (value_type != GW_EVENT_VALUE_NONE) flags |= GW_EVENT_PAYLOAD_HAS_VALUE;
    gw_event_bus_publish_internal(type, source, device_uid, short_addr, msg, flags, endpoint, cmd, cluster_id, attr_id,
                                  value_type, value_bool, value_i64, value_f64, value_text, payload_cbor, payload_len);
}

void gw_event_bus_publish_ex(const char *type,
                            const char *source,
                            const char *device_uid,
                            uint16_t short_addr,
                            const char *msg,
                            const uint8_t *payload_cbor,
                            size_t payload_len)
{
    gw_event_bus_publish_internal(type, source, device_uid, short_addr, msg, 0, 0, NULL, 0, 0,
                                  GW_EVENT_VALUE_NONE, false, 0, 0.0, NULL, payload_cbor, payload_len);
}

static void gw_event_bus_publish_internal(const char *type,
                                          const char *source,
                                          const char *device_uid,
                                          uint16_t short_addr,
                                          const char *msg,
                                          uint8_t payload_flags,
                                          uint8_t endpoint,
                                          const char *cmd,
                                          uint16_t cluster_id,
                                          uint16_t attr_id,
                                          gw_event_value_type_t value_type,
                                          bool value_bool,
                                          int64_t value_i64,
                                          double value_f64,
                                          const char *value_text,
                                          const uint8_t *payload_cbor,
                                          size_t payload_len)
{
    if (!s_inited) {
        return;
    }
    (void)payload_cbor;
    (void)payload_len;

    gw_event_t e = {0};
    e.v = 1;
    e.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    safe_copy_str(e.type, sizeof(e.type), type);
    safe_copy_str(e.source, sizeof(e.source), source);
    safe_copy_str(e.device_uid, sizeof(e.device_uid), device_uid);
    e.short_addr = short_addr;
    safe_copy_str(e.msg, sizeof(e.msg), msg);
    e.payload_flags = payload_flags;
    e.payload_endpoint = endpoint;
    e.payload_cluster = cluster_id;
    e.payload_attr = attr_id;
    safe_copy_str(e.payload_cmd, sizeof(e.payload_cmd), cmd);
    e.payload_value_type = (uint8_t)value_type;
    e.payload_value_bool = value_bool ? 1 : 0;
    e.payload_value_i64 = value_i64;
    e.payload_value_f64 = value_f64;
    safe_copy_str(e.payload_value_text, sizeof(e.payload_value_text), value_text);

    portENTER_CRITICAL(&s_id_lock);
    e.id = s_next_id++;
    portEXIT_CRITICAL(&s_id_lock);

    // Notify listeners outside of the ring critical section.
    gw_event_listener_slot_t listeners[GW_EVENT_LISTENER_CAP];
    size_t listener_count = 0;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb) {
            listeners[listener_count++] = s_listeners[i];
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < listener_count; i++) {
        listeners[i].cb(&e, listeners[i].user_ctx);
    }

    // Duplicate event log + ring insert for UI/debugging (async when possible).
    if (s_out_q && event_should_go_to_out_queue(e.type) && xQueueSend(s_out_q, &e, 0) == pdTRUE) {
        return;
    }

    gw_event_bus_record_event(&e);
}

size_t gw_event_bus_list_since(uint32_t since_id, gw_event_t *out, size_t max_out, uint32_t *out_last_id)
{
    (void)since_id;
    (void)out;
    (void)max_out;
    if (out_last_id) {
        *out_last_id = gw_event_bus_last_id();
    }
    return 0;
}

esp_err_t gw_event_bus_add_listener(gw_event_bus_listener_t cb, void *user_ctx)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == NULL) {
            s_listeners[i].cb = cb;
            s_listeners[i].user_ctx = user_ctx;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t gw_event_bus_remove_listener(gw_event_bus_listener_t cb, void *user_ctx)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            s_listeners[i].cb = NULL;
            s_listeners[i].user_ctx = NULL;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NOT_FOUND;
}

void gw_event_bus_set_out_queue(QueueHandle_t q)
{
    s_out_q = q;
}

void gw_event_bus_record_event(const gw_event_t *e)
{
    if (!e) {
        return;
    }
}

