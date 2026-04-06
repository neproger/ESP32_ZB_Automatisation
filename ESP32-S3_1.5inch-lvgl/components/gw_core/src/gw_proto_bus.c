#include "gw_core/gw_proto_bus.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "gw_proto_bus";
static bool s_inited;

#define GW_PROTO_BUS_LISTENER_CAP 6
typedef struct {
    gw_proto_bus_listener_t cb;
    uint8_t channel_mask;
    void *user_ctx;
} gw_proto_bus_listener_slot_t;

static gw_proto_bus_listener_slot_t s_listeners[GW_PROTO_BUS_LISTENER_CAP];
static portMUX_TYPE s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t gw_proto_bus_init(void)
{
    portENTER_CRITICAL(&s_listener_lock);
    memset(s_listeners, 0, sizeof(s_listeners));
    s_inited = true;
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_OK;
}

esp_err_t gw_proto_bus_add_listener(gw_proto_bus_listener_t cb, uint8_t channel_mask, void *user_ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_PROTO_BUS_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < GW_PROTO_BUS_LISTENER_CAP; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i].cb = cb;
            s_listeners[i].channel_mask = channel_mask ? channel_mask : GW_PROTO_BUS_CHANNEL_ANY;
            s_listeners[i].user_ctx = user_ctx;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t gw_proto_bus_remove_listener(gw_proto_bus_listener_t cb, void *user_ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_PROTO_BUS_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            s_listeners[i].cb = NULL;
            s_listeners[i].channel_mask = 0;
            s_listeners[i].user_ctx = NULL;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t gw_proto_bus_publish(gw_proto_bus_channel_t channel, const gw_proto_hdr_t *hdr, const void *payload)
{
    if (!hdr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hdr->len > 0 && !payload) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "proto_bus_publish: channel=%d type=0x%02x len=%d", channel, hdr->type, hdr->len);

    gw_proto_bus_listener_slot_t listeners[GW_PROTO_BUS_LISTENER_CAP];
    size_t listener_count = 0;

    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_PROTO_BUS_LISTENER_CAP; i++) {
        if (s_listeners[i].cb && (s_listeners[i].channel_mask & (uint8_t)channel)) {
            listeners[listener_count++] = s_listeners[i];
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);

    ESP_LOGI(TAG, "proto_bus_publish: found %d listeners", listener_count);

    for (size_t i = 0; i < listener_count; i++) {
        listeners[i].cb(channel, hdr, payload, listeners[i].user_ctx);
    }

    return ESP_OK;
}
