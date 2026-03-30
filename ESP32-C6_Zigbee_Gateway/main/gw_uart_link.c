#include "gw_uart_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_zigbee_gateway.h"
#include "gw_core/device_registry.h"
#include "gw_core/device_storage.h"
#include "gw_core/event_bus.h"
#include "gw_core/gw_proto.h"
#include "gw_core/gw_uart_proto.h"
#include "gw_core/state_store.h"
#include "gw_core/types.h"
#include "gw_proto/gw_proto_uart.h"
#include "gw_zigbee/gw_zigbee.h"

#define GW_UART_PORT UART_NUM_1
#define GW_UART_BAUD 230400
#define GW_UART_RX_BUF_SIZE 1024
#define GW_UART_TX_BUF_SIZE 1024
#define GW_UART_EVT_Q_LEN 16
#define GW_UART_TX_EVENT_Q 24

static const char *TAG = "gw_uart";

static QueueHandle_t s_evt_q;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_rx_task;
static TaskHandle_t s_snapshot_task;
static SemaphoreHandle_t s_tx_lock;
static uint16_t s_evt_seq = 1;
static volatile bool s_snapshot_requested;
static volatile bool s_snapshot_tx_active;

static bool uart_write_all(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return true;
    }

    size_t off = 0;
    int attempts = 0;
    while (off < len && attempts < 8) {
        int wr = uart_write_bytes(GW_UART_PORT, data + off, len - off);
        if (wr > 0) {
            off += (size_t)wr;
            attempts = 0;
            continue;
        }
        attempts++;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (off < len) {
        ESP_LOGW(TAG, "UART short write: sent=%u total=%u", (unsigned)off, (unsigned)len);
        return false;
    }

    (void)uart_wait_tx_done(GW_UART_PORT, pdMS_TO_TICKS(20));
    return true;
}
static void uart_send_frame(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len);
static void snapshot_request_async(void);

#if defined(UART_SCLK_XTAL)
#define GW_UART_SCLK_SRC UART_SCLK_XTAL
#else
#define GW_UART_SCLK_SRC UART_SCLK_DEFAULT
#endif

static const char *msg_type_name(uint8_t t)
{
    switch (t) {
        case GW_UART_MSG_HELLO:
            return "HELLO";
        case GW_UART_MSG_HELLO_ACK:
            return "HELLO_ACK";
        case GW_UART_MSG_PING:
            return "PING";
        case GW_UART_MSG_PONG:
            return "PONG";
        case GW_UART_MSG_CMD_REQ:
            return "CMD_REQ";
        case GW_UART_MSG_CMD_RSP:
            return "CMD_RSP";
        case GW_UART_MSG_EVT:
            return "EVT";
        case GW_PROTO_MSG_SYNC_BEGIN:
            return "PROTO_SYNC_BEGIN";
        case GW_PROTO_MSG_SYNC_END:
            return "PROTO_SYNC_END";
        case GW_PROTO_MSG_DEVICE_UPSERT:
            return "PROTO_DEVICE_UPSERT";
        case GW_PROTO_MSG_DEVICE_REMOVE:
            return "PROTO_DEVICE_REMOVE";
        case GW_PROTO_MSG_ENDPOINT_UPSERT:
            return "PROTO_ENDPOINT_UPSERT";
        case GW_PROTO_MSG_ENDPOINT_REMOVE:
            return "PROTO_ENDPOINT_REMOVE";
        case GW_PROTO_MSG_STATE_ITEM:
            return "PROTO_STATE_ITEM";
        case GW_PROTO_MSG_STATE_REMOVE:
            return "PROTO_STATE_REMOVE";
        case GW_PROTO_MSG_GROUP_UPSERT:
            return "PROTO_GROUP_UPSERT";
        case GW_PROTO_MSG_GROUP_REMOVE:
            return "PROTO_GROUP_REMOVE";
        case GW_PROTO_MSG_GROUP_ITEM_UPSERT:
            return "PROTO_GROUP_ITEM_UPSERT";
        case GW_PROTO_MSG_GROUP_ITEM_REMOVE:
            return "PROTO_GROUP_ITEM_REMOVE";
        case GW_PROTO_MSG_SETTINGS:
            return "PROTO_SETTINGS";
        default:
            return "UNKNOWN";
    }
}

static bool is_forwardable_event(const char *type)
{
    if (!type || !type[0]) {
        return false;
    }
    if (strncmp(type, "zigbee.", 7) == 0) {
        return true;
    }
    if (strncmp(type, "zigbee_", 7) == 0) {
        return true;
    }
    if (strcmp(type, "device.join") == 0 || strcmp(type, "device.leave") == 0) {
        return true;
    }
    if (strcmp(type, "device.changed") == 0) {
        return true;
    }
    if (strncmp(type, "system.", 7) == 0) {
        return true;
    }
    if (strncmp(type, "system_", 7) == 0) {
        return true;
    }
    return false;
}

static uint16_t clamp_u16_i32(int32_t v)
{
    if (v <= 0) {
        return 0;
    }
    if (v >= 65535) {
        return 65535;
    }
    return (uint16_t)v;
}

static gw_uart_status_t map_esp_err_to_status(esp_err_t err)
{
    if (err == ESP_OK) {
        return GW_UART_STATUS_OK;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return GW_UART_STATUS_INVALID_ARGS;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return GW_UART_STATUS_NOT_READY;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return GW_UART_STATUS_NOT_FOUND;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return GW_UART_STATUS_UNSUPPORTED;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return GW_UART_STATUS_TIMEOUT;
    }
    if (err == ESP_ERR_NO_MEM) {
        return GW_UART_STATUS_BUSY;
    }
    return GW_UART_STATUS_INTERNAL_ERROR;
}

static bool uid_matches(const gw_device_uid_t *uid, const char *uid_str)
{
    if (!uid || !uid_str) {
        return false;
    }
    return strncmp(uid->uid, uid_str, sizeof(uid->uid)) == 0;
}

static void uart_send_proto_sync_begin(uint16_t seq, uint32_t total_records)
{
    gw_proto_sync_begin_v1_t msg = {
        .scope = GW_PROTO_SYNC_SCOPE_FULL,
        .reserved0 = 0,
        .reserved1 = 0,
        .total_records = total_records,
    };
    uart_send_frame(GW_PROTO_MSG_SYNC_BEGIN, seq, &msg, sizeof(msg));
}

static void uart_send_proto_sync_end(uint16_t seq, uint32_t total_records)
{
    gw_proto_sync_end_v1_t msg = {
        .scope = GW_PROTO_SYNC_SCOPE_FULL,
        .status = 0,
        .reserved0 = 0,
        .total_records = total_records,
    };
    uart_send_frame(GW_PROTO_MSG_SYNC_END, seq, &msg, sizeof(msg));
}

static void uart_send_proto_device_record(const gw_device_full_t *device, uint16_t seq)
{
    gw_proto_device_v1_t msg = {0};
    if (!device) {
        return;
    }

    msg.device_uid = device->device_uid;
    msg.short_addr = device->short_addr;
    strlcpy(msg.name, device->name, sizeof(msg.name));
    msg.version = 0;
    msg.last_seen_ms = device->last_seen_ms;
    msg.has_onoff = device->has_onoff ? 1u : 0u;
    msg.has_button = device->has_button ? 1u : 0u;
    uart_send_frame(GW_PROTO_MSG_DEVICE_UPSERT, seq, &msg, sizeof(msg));
}

static void uart_send_proto_endpoint_record(const gw_device_full_t *device,
                                            uint8_t endpoint,
                                            const gw_device_endpoint_t *ep,
                                            uint16_t seq)
{
    gw_proto_endpoint_v1_t msg = {0};
    if (!device || !ep) {
        return;
    }

    msg.uid = device->device_uid;
    msg.short_addr = device->short_addr;
    msg.endpoint = endpoint;
    msg.version = 0;
    msg.profile_id = ep->profile_id;
    msg.device_id = ep->device_id;
    msg.in_cluster_count = ep->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : ep->in_cluster_count;
    msg.out_cluster_count = ep->out_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : ep->out_cluster_count;
    if (msg.in_cluster_count > 0) {
        memcpy(msg.in_clusters, ep->in_clusters, msg.in_cluster_count * sizeof(uint16_t));
    }
    if (msg.out_cluster_count > 0) {
        memcpy(msg.out_clusters, ep->out_clusters, msg.out_cluster_count * sizeof(uint16_t));
    }
    uart_send_frame(GW_PROTO_MSG_ENDPOINT_UPSERT, seq, &msg, sizeof(msg));
}

static void uart_send_proto_state_record(const gw_state_item_t *item, uint16_t seq)
{
    gw_proto_state_item_v1_t msg = {0};
    if (!item) {
        return;
    }

    msg.uid = item->uid;
    msg.endpoint = 0;
    msg.value_type = (uint8_t)item->value_type;
    strlcpy(msg.key, item->key, sizeof(msg.key));
    msg.version = 0;
    msg.value_bool = item->value_bool ? 1u : 0u;
    msg.value_f32 = item->value_f32;
    msg.value_u32 = item->value_u32;
    msg.value_u64 = item->value_u64;
    msg.ts_ms = item->ts_ms;
    uart_send_frame(GW_PROTO_MSG_STATE_ITEM, seq, &msg, sizeof(msg));
}

static size_t count_state_items_for_uid(const gw_device_uid_t *uid)
{
    size_t count = 0;
    const size_t state_count = gw_state_store_count();
    if (!uid) {
        return 0;
    }

    for (size_t i = 0; i < state_count; ++i) {
        gw_state_item_t item = {0};
        if (gw_state_store_get_by_index(i, &item) != ESP_OK) {
            continue;
        }
        if (uid_matches(&item.uid, uid->uid)) {
            count++;
        }
    }
    return count;
}

static void uart_send_proto_states_for_uid(const gw_device_uid_t *uid, uint16_t seq)
{
    const size_t state_count = gw_state_store_count();
    if (!uid) {
        return;
    }

    for (size_t i = 0; i < state_count; ++i) {
        gw_state_item_t item = {0};
        if (gw_state_store_get_by_index(i, &item) != ESP_OK) {
            continue;
        }
        if (!uid_matches(&item.uid, uid->uid)) {
            continue;
        }
        uart_send_proto_state_record(&item, seq);
    }
}

static void uart_send_proto_device_remove(const char *uid_str, uint16_t seq)
{
    gw_proto_device_remove_v1_t msg = {0};
    if (!uid_str || uid_str[0] == '\0') {
        return;
    }
    strlcpy(msg.device_uid.uid, uid_str, sizeof(msg.device_uid.uid));
    uart_send_frame(GW_PROTO_MSG_DEVICE_REMOVE, seq, &msg, sizeof(msg));
}

static void uart_send_snapshot_device_delta(const char *uid_str, uint16_t seq)
{
    if (!uid_str || uid_str[0] == '\0') {
        return;
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, uid_str, sizeof(uid.uid));
    gw_device_full_t device = {0};
    if (gw_device_storage_get(&uid, &device) != ESP_OK) {
        return;
    }

    uart_send_proto_device_record(&device, seq);

    for (uint8_t ep_idx = 0; ep_idx < device.endpoint_count && ep_idx < GW_DEVICE_MAX_ENDPOINTS; ++ep_idx) {
        const uint8_t endpoint = (uint8_t)(ep_idx + 1u);
        const gw_device_endpoint_t *ep = &device.endpoints[ep_idx];
        if (ep->profile_id == 0 && ep->device_id == 0 && ep->in_cluster_count == 0 && ep->out_cluster_count == 0) {
            continue;
        }
        uart_send_proto_endpoint_record(&device, endpoint, ep, seq);
    }
    uart_send_proto_states_for_uid(&device.device_uid, seq);
}

static esp_err_t uart_send_snapshot(uint16_t base_seq)
{
    const size_t dev_count = gw_device_storage_count();
    size_t endpoint_count = 0;
    size_t state_count = 0;

    for (size_t di = 0; di < dev_count; ++di) {
        gw_device_full_t device = {0};
        if (gw_device_storage_get_by_index(di, &device) != ESP_OK) {
            continue;
        }
        for (uint8_t ep_idx = 0; ep_idx < device.endpoint_count && ep_idx < GW_DEVICE_MAX_ENDPOINTS; ++ep_idx) {
            const gw_device_endpoint_t *ep = &device.endpoints[ep_idx];
            if (ep->profile_id == 0 && ep->device_id == 0 && ep->in_cluster_count == 0 && ep->out_cluster_count == 0) {
                continue;
            }
            endpoint_count++;
        }
        state_count += count_state_items_for_uid(&device.device_uid);
    }

    uart_send_proto_sync_begin(base_seq, (uint32_t)(dev_count + endpoint_count + state_count));

    // Phase 1: send only device topology roots first.
    for (size_t di = 0; di < dev_count; ++di) {
        gw_device_full_t device = {0};
        if (gw_device_storage_get_by_index(di, &device) != ESP_OK) {
            continue;
        }
        uart_send_proto_device_record(&device, base_seq);
    }

    // Phase 2: stream endpoint metadata followed by cached state for the same device.
    for (size_t di = 0; di < dev_count; ++di) {
        gw_device_full_t device = {0};
        if (gw_device_storage_get_by_index(di, &device) != ESP_OK) {
            continue;
        }

        for (uint8_t ep_idx = 0; ep_idx < device.endpoint_count && ep_idx < GW_DEVICE_MAX_ENDPOINTS; ++ep_idx) {
            const uint8_t endpoint = (uint8_t)(ep_idx + 1u);
            const gw_device_endpoint_t *ep = &device.endpoints[ep_idx];
            if (ep->profile_id == 0 && ep->device_id == 0 && ep->in_cluster_count == 0 && ep->out_cluster_count == 0) {
                continue;
            }
            uart_send_proto_endpoint_record(&device, endpoint, ep, base_seq);
        }
        uart_send_proto_states_for_uid(&device.device_uid, base_seq);
    }

    uart_send_proto_sync_end(base_seq, (uint32_t)(dev_count + endpoint_count + state_count));
    ESP_LOGI(TAG, "Proto snapshot sent: devices=%u endpoints=%u states=%u", (unsigned)dev_count, (unsigned)endpoint_count, (unsigned)state_count);
    return ESP_OK;
}

static void snapshot_request_async(void)
{
    s_snapshot_requested = true;
    if (s_snapshot_task) {
        xTaskNotifyGive(s_snapshot_task);
    }
}

static void uart_send_frame(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len)
{
    gw_proto_hdr_t hdr = {
        .version = GW_PROTO_VERSION_V1,
        .type = msg_type,
        .len = payload_len,
        .seq = seq,
        .reserved = 0,
    };
    uint8_t raw[GW_PROTO_UART_MAX_FRAME_SIZE];
    size_t raw_len = 0;

    if (payload_len > GW_PROTO_UART_MAX_PAYLOAD) {
        return;
    }
    if (gw_proto_uart_build_frame(&hdr, payload, payload_len, raw, sizeof(raw), &raw_len) != ESP_OK) {
        return;
    }
    if (msg_type == GW_UART_MSG_EVT) {
        ESP_LOGD(TAG, "UART TX %s seq=%u payload=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)payload_len);
    } else {
        ESP_LOGI(TAG, "UART TX %s seq=%u payload=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)payload_len);
    }
    if (s_tx_lock) {
        (void)xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    bool ok = uart_write_all(raw, raw_len);
    if (s_tx_lock) {
        (void)xSemaphoreGive(s_tx_lock);
    }
    if (!ok) {
        ESP_LOGW(TAG, "UART TX drop %s seq=%u len=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)raw_len);
    }
}

static void uart_send_cmd_rsp(uint16_t seq, uint32_t req_id, gw_uart_status_t status, esp_err_t err)
{
    gw_uart_cmd_rsp_v1_t rsp = {
        .req_id = req_id,
        .status = (uint16_t)status,
        .zb_status = 0,
    };

    strlcpy(rsp.message, (err == ESP_OK) ? "ok" : esp_err_to_name(err), sizeof(rsp.message));
    uart_send_frame(GW_UART_MSG_CMD_RSP, seq, &rsp, sizeof(rsp));
}

static uint8_t map_evt_kind(const char *type)
{
    if (!type) {
        return GW_UART_EVT_NET_STATE;
    }
    if (strcmp(type, "zigbee.attr_report") == 0) {
        return GW_UART_EVT_ATTR_REPORT;
    }
    if (strcmp(type, "zigbee.command") == 0) {
        return GW_UART_EVT_COMMAND;
    }
    if (strstr(type, "join") != NULL) {
        return GW_UART_EVT_DEVICE_JOIN;
    }
    if (strstr(type, "leave") != NULL) {
        return GW_UART_EVT_DEVICE_LEAVE;
    }
    return GW_UART_EVT_NET_STATE;
}

static uint8_t map_evt_value_type(uint8_t t)
{
    switch ((gw_event_value_type_t)t) {
        case GW_EVENT_VALUE_BOOL:
            return GW_UART_VALUE_BOOL;
        case GW_EVENT_VALUE_I64:
            return GW_UART_VALUE_I64;
        case GW_EVENT_VALUE_F64:
            return GW_UART_VALUE_F32;
        case GW_EVENT_VALUE_TEXT:
            return GW_UART_VALUE_TEXT;
        case GW_EVENT_VALUE_NONE:
        default:
            return GW_UART_VALUE_NONE;
    }
}

static void uart_send_event(const gw_event_t *e)
{
    gw_uart_evt_v1_t evt;
    memset(&evt, 0, sizeof(evt));

    evt.event_id = e->id;
    evt.ts_ms = e->ts_ms;
    evt.evt_id = map_evt_kind(e->type);
    strlcpy(evt.event_type, e->type, sizeof(evt.event_type));
    if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_CMD) {
        strlcpy(evt.cmd, e->payload_cmd, sizeof(evt.cmd));
    }
    strlcpy(evt.device_uid, e->device_uid, sizeof(evt.device_uid));
    evt.short_addr = e->short_addr;
    evt.endpoint = e->payload_endpoint;
    evt.cluster_id = e->payload_cluster;
    evt.attr_id = e->payload_attr;
    evt.value_type = map_evt_value_type(e->payload_value_type);
    evt.value_bool = e->payload_value_bool;
    evt.value_i64 = e->payload_value_i64;
    evt.value_f32 = (float)e->payload_value_f64;
    strlcpy(evt.value_text, e->payload_value_text, sizeof(evt.value_text));

    uart_send_frame(GW_UART_MSG_EVT, s_evt_seq++, &evt, sizeof(evt));
}

static void on_event(const gw_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event || !s_evt_q) {
        return;
    }
    if (!is_forwardable_event(event->type)) {
        return;
    }
    // During snapshot transfer keep UART line dedicated to stream frames.
    if (s_snapshot_tx_active) {
        return;
    }
    if (strcmp(event->type, "device.join") == 0 && event->device_uid[0] != '\0') {
        uart_send_snapshot_device_delta(event->device_uid, s_evt_seq++);
    }
    if (strcmp(event->type, "device.leave") == 0 && event->device_uid[0] != '\0') {
        uart_send_proto_device_remove(event->device_uid, s_evt_seq++);
    }
    (void)xQueueSend(s_evt_q, event, 0);
}

static esp_err_t exec_cmd_req(const gw_uart_cmd_req_v1_t *req)
{
    gw_device_uid_t uid = {0};
    bool has_uid = req->device_uid[0] != '\0';

    if (has_uid) {
        strlcpy(uid.uid, req->device_uid, sizeof(uid.uid));
    }

    switch ((gw_uart_cmd_id_t)req->cmd_id) {
        case GW_UART_CMD_PERMIT_JOIN: {
            if (req->param0 < 0 || req->param0 > 255) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_zigbee_permit_join((uint8_t)req->param0);
        }

        case GW_UART_CMD_ONOFF: {
            if (!has_uid || req->endpoint == 0 || req->param0 < 0 || req->param0 > 2) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_zigbee_onoff_cmd(&uid, req->endpoint, (gw_zigbee_onoff_cmd_t)req->param0);
        }

        case GW_UART_CMD_LEVEL: {
            if (!has_uid || req->endpoint == 0 || req->param0 < 0 || req->param0 > 254 || req->param1 < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_level_t lv = {
                .level = (uint8_t)req->param0,
                .transition_ms = clamp_u16_i32(req->param1 * 100),
            };
            return gw_zigbee_level_move_to_level(&uid, req->endpoint, lv);
        }

        case GW_UART_CMD_COLOR_XY: {
            if (!has_uid || req->endpoint == 0 || req->param0 < 0 || req->param1 < 0 || req->param2 < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_xy_t xy = {
                .x = clamp_u16_i32(req->param0),
                .y = clamp_u16_i32(req->param1),
                .transition_ms = clamp_u16_i32(req->param2 * 100),
            };
            return gw_zigbee_color_move_to_xy(&uid, req->endpoint, xy);
        }

        case GW_UART_CMD_COLOR_TEMP: {
            if (!has_uid || req->endpoint == 0 || req->param0 <= 0 || req->param1 < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_temp_t ct = {
                .mireds = clamp_u16_i32(req->param0),
                .transition_ms = clamp_u16_i32(req->param1 * 100),
            };
            return gw_zigbee_color_move_to_temp(&uid, req->endpoint, ct);
        }

        case GW_UART_CMD_READ_ATTR: {
            // attr_id can validly be 0x0000 for many clusters (onoff/level/etc).
            if (!has_uid || req->endpoint == 0 || req->cluster_id == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_zigbee_read_attr(&uid, req->endpoint, req->cluster_id, req->attr_id);
        }

        case GW_UART_CMD_WRITE_ATTR:
        case GW_UART_CMD_IDENTIFY:
            return ESP_ERR_NOT_SUPPORTED;

        case GW_UART_CMD_SYNC_SNAPSHOT:
            snapshot_request_async();
            return ESP_OK;
        case GW_UART_CMD_SET_DEVICE_NAME: {
            if (!has_uid) {
                return ESP_ERR_INVALID_ARG;
            }
            char name_buf[sizeof(req->value_text)] = {0};
            strlcpy(name_buf, req->value_text, sizeof(name_buf));
            return gw_device_registry_set_name(&uid, name_buf);
        }
        case GW_UART_CMD_REMOVE_DEVICE: {
            if (!has_uid) {
                return ESP_ERR_INVALID_ARG;
            }
            gw_device_full_t device = {0};
            esp_err_t err = gw_device_storage_get(&uid, &device);
            if (err != ESP_OK) {
                return err;
            }
            if (device.short_addr == 0) {
                return ESP_ERR_INVALID_STATE;
            }
            return gw_zigbee_device_leave(&uid, device.short_addr, false);
        }
        case GW_UART_CMD_REMOVE_ALL_DEVICES: {
            size_t iterations = 0;
            while (iterations < GW_DEVICE_MAX_DEVICES) {
                gw_device_full_t device = {0};
                if (gw_device_storage_get_by_index(0, &device) != ESP_OK) {
                    break;
                }
                if (device.device_uid.uid[0] == '\0') {
                    break;
                }
                if (device.short_addr != 0) {
                    (void)gw_zigbee_device_leave(&device.device_uid, device.short_addr, false);
                }
                (void)gw_zb_model_remove_device(&device.device_uid);
                (void)gw_state_store_remove_uid(&device.device_uid);
                (void)gw_device_registry_remove(&device.device_uid);
                uart_send_proto_device_remove(device.device_uid.uid, s_evt_seq++);
                iterations++;
            }
            return ESP_OK;
        }
        case GW_UART_CMD_WIFI_CONFIG_SET:
            return ESP_ERR_NOT_SUPPORTED;
        case GW_UART_CMD_NET_SERVICES_START:
            return ESP_ERR_NOT_SUPPORTED;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static void handle_cmd_req(const gw_proto_uart_frame_t *frame)
{
    gw_uart_cmd_req_v1_t req;
    memset(&req, 0, sizeof(req));

    size_t copy_len = frame->hdr.len < sizeof(req) ? frame->hdr.len : sizeof(req);
    if (copy_len > 0) {
        memcpy(&req, frame->payload, copy_len);
    }
    req.device_uid[sizeof(req.device_uid) - 1] = '\0';
    req.value_text[sizeof(req.value_text) - 1] = '\0';
    req.value_blob[sizeof(req.value_blob) - 1] = '\0';

    uint32_t req_id = req.req_id ? req.req_id : frame->hdr.seq;
    if ((gw_uart_cmd_id_t)req.cmd_id == GW_UART_CMD_SYNC_SNAPSHOT) {
        ESP_LOGI(TAG,
                 "%s requested (seq=%u req_id=%u)",
                 "SYNC_SNAPSHOT",
                 (unsigned)frame->hdr.seq,
                 (unsigned)req_id);
        uart_send_cmd_rsp(frame->hdr.seq, req_id, GW_UART_STATUS_OK, ESP_OK);
        snapshot_request_async();
        return;
    }

    esp_err_t err = exec_cmd_req(&req);
    gw_uart_status_t st = map_esp_err_to_status(err);
    uart_send_cmd_rsp(frame->hdr.seq, req_id, st, err);
}

static void handle_rx_frame(const gw_proto_uart_frame_t *frame)
{
    if (frame->hdr.type == GW_UART_MSG_EVT) {
        ESP_LOGD(TAG, "UART RX %s seq=%u payload=%u", msg_type_name(frame->hdr.type), (unsigned)frame->hdr.seq, (unsigned)frame->hdr.len);
    } else {
        ESP_LOGI(TAG, "UART RX %s seq=%u payload=%u", msg_type_name(frame->hdr.type), (unsigned)frame->hdr.seq, (unsigned)frame->hdr.len);
    }

    switch (frame->hdr.type) {
        case GW_UART_MSG_PING:
            uart_send_frame(GW_UART_MSG_PONG, frame->hdr.seq, NULL, 0);
            break;
        case GW_UART_MSG_HELLO:
            uart_send_frame(GW_UART_MSG_HELLO_ACK, frame->hdr.seq, NULL, 0);
            break;
        case GW_UART_MSG_CMD_REQ:
            handle_cmd_req(frame);
            break;
        default:
            break;
    }
}

static void uart_tx_task(void *arg)
{
    (void)arg;
    gw_event_t e;
    for (;;) {
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) == pdTRUE) {
            uart_send_event(&e);
        }
    }
}

static void uart_snapshot_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_snapshot_requested) {
            continue;
        }
        while (s_snapshot_requested) {
            s_snapshot_requested = false;
            s_snapshot_tx_active = true;
            (void)uart_send_snapshot((uint16_t)(s_evt_seq++));
            s_snapshot_tx_active = false;
        }
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    gw_proto_uart_parser_t parser;
    gw_proto_uart_parser_init(&parser);

    for (;;) {
        int n = uart_read_bytes(GW_UART_PORT, rx, sizeof(rx), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        size_t off = 0;
        while (off < (size_t)n) {
            gw_proto_uart_frame_t frame;
            bool ready = false;
            size_t consumed = 0;
            esp_err_t err = gw_proto_uart_parser_feed(&parser, &rx[off], (size_t)n - off, &frame, &ready, &consumed);
            if (consumed == 0) {
                break;
            }
            off += consumed;

            if (err != ESP_OK) {
                if (err == ESP_ERR_INVALID_CRC) {
                    ESP_LOGW(TAG, "UART frame CRC error");
                } else {
                    ESP_LOGW(TAG, "UART frame parse error: %s", esp_err_to_name(err));
                }
                continue;
            }
            if (ready) {
                handle_rx_frame(&frame);
            }
        }
    }
}

esp_err_t gw_uart_link_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = GW_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = GW_UART_SCLK_SRC,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(GW_UART_PORT, GW_UART_RX_BUF_SIZE, GW_UART_TX_BUF_SIZE, GW_UART_EVT_Q_LEN, NULL, 0), TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(GW_UART_PORT, &cfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(GW_UART_PORT, GW_UART_TX_PIN, GW_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin failed");

    s_evt_q = xQueueCreate(GW_UART_TX_EVENT_Q, sizeof(gw_event_t));
    if (!s_evt_q) {
        return ESP_ERR_NO_MEM;
    }
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(gw_event_bus_add_listener(on_event, NULL), TAG, "gw_event_bus_add_listener failed");

    if (xTaskCreate(uart_tx_task, "uart_tx", 4096, NULL, 6, &s_tx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uart_snapshot_task, "uart_snap", 9216, NULL, 6, &s_snapshot_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 6, &s_rx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART binary link started: UART1 TX=GPIO%d RX=GPIO%d baud=%d", GW_UART_TX_PIN, GW_UART_RX_PIN, GW_UART_BAUD);
    return ESP_OK;
}





