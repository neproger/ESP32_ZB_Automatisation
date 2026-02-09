#include "gw_uart_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_zigbee_gateway.h"
#include "gw_core/device_registry.h"
#include "gw_core/device_storage.h"
#include "gw_core/event_bus.h"
#include "gw_core/gw_uart_proto.h"
#include "gw_core/sensor_store.h"
#include "gw_core/state_store.h"
#include "gw_core/types.h"
#include "gw_core/zb_model.h"
#include "gw_zigbee/gw_zigbee.h"

#define GW_UART_PORT UART_NUM_1
#define GW_UART_BAUD 460800
#define GW_UART_RX_BUF_SIZE 1024
#define GW_UART_TX_BUF_SIZE 1024
#define GW_UART_EVT_Q_LEN 16
#define GW_UART_TX_EVENT_Q 24

static const char *TAG = "gw_uart";

static QueueHandle_t s_evt_q;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_rx_task;
static TaskHandle_t s_snapshot_task;
static uint16_t s_evt_seq = 1;
static volatile bool s_snapshot_requested;

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
static bool endpoint_has_in_cluster(const gw_zb_endpoint_t *ep, uint16_t cluster_id);
static void snapshot_request_async(void);

#if defined(UART_SCLK_XTAL)
#define GW_UART_SCLK_SRC UART_SCLK_XTAL
#else
#define GW_UART_SCLK_SRC UART_SCLK_DEFAULT
#endif

static const char *msg_type_name(uint8_t t)
{
    switch ((gw_uart_msg_type_t)t) {
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
        case GW_UART_MSG_SNAPSHOT:
            return "SNAPSHOT";
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

static void uart_send_snapshot_frame(const gw_uart_snapshot_v1_t *snap, uint16_t seq)
{
    if (!snap) {
        return;
    }
    uart_send_frame(GW_UART_MSG_SNAPSHOT, seq, snap, sizeof(*snap));
}

static bool uid_equals_str(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    return strncmp(a, b, GW_DEVICE_UID_STRLEN) == 0;
}

static bool endpoint_has_cluster_arr(const uint16_t *clusters, uint8_t count, uint16_t cluster_id)
{
    if (!clusters || cluster_id == 0) {
        return false;
    }
    size_t n = count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : count;
    for (size_t i = 0; i < n; i++) {
        if (clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static size_t build_live_device_view(const gw_zb_endpoint_t *all_eps,
                                     size_t all_count,
                                     gw_device_t *out_devices,
                                     size_t max_devices)
{
    if (!all_eps || !out_devices || max_devices == 0) {
        return 0;
    }

    size_t dev_count = 0;
    for (size_t i = 0; i < all_count; i++) {
        if (all_eps[i].uid.uid[0] == '\0') {
            continue;
        }
        size_t idx = (size_t)-1;
        for (size_t d = 0; d < dev_count; d++) {
            if (uid_equals_str(out_devices[d].device_uid.uid, all_eps[i].uid.uid)) {
                idx = d;
                break;
            }
        }

        if (idx == (size_t)-1) {
            if (dev_count >= max_devices) {
                break;
            }
            idx = dev_count++;
            memset(&out_devices[idx], 0, sizeof(out_devices[idx]));
            strlcpy(out_devices[idx].device_uid.uid, all_eps[i].uid.uid, sizeof(out_devices[idx].device_uid.uid));
            out_devices[idx].short_addr = all_eps[i].short_addr;

            // Enrich from registry if present; live model remains primary source.
            gw_device_t reg = {0};
            if (gw_device_registry_get(&out_devices[idx].device_uid, &reg) == ESP_OK) {
                if (reg.name[0]) {
                    strlcpy(out_devices[idx].name, reg.name, sizeof(out_devices[idx].name));
                }
                out_devices[idx].last_seen_ms = reg.last_seen_ms;
                out_devices[idx].has_onoff = reg.has_onoff;
                out_devices[idx].has_button = reg.has_button;
                if (reg.short_addr != 0) {
                    out_devices[idx].short_addr = reg.short_addr;
                }
            }
        }

        if (!out_devices[idx].has_onoff &&
            endpoint_has_cluster_arr(all_eps[i].in_clusters, all_eps[i].in_cluster_count, 0x0006)) {
            out_devices[idx].has_onoff = true;
        }
    }
    return dev_count;
}

static void queue_read_attr_if_cluster(const gw_device_uid_t *uid,
                                       const gw_zb_endpoint_t *ep,
                                       uint16_t cluster_id,
                                       uint16_t attr_id,
                                       uint32_t *req_count)
{
    if (!uid || !ep) {
        return;
    }
    if (endpoint_has_in_cluster(ep, cluster_id)) {
        if (gw_zigbee_read_attr(uid, ep->endpoint, cluster_id, attr_id) == ESP_OK && req_count) {
            (*req_count)++;
        }
    }
}

static void uart_refresh_states_before_snapshot(void)
{
    gw_zb_endpoint_t *all_eps = (gw_zb_endpoint_t *)calloc(GW_ZB_MAX_ENDPOINTS, sizeof(gw_zb_endpoint_t));
    if (!all_eps) {
        ESP_LOGW(TAG, "snapshot refresh: no mem for endpoint list");
        return;
    }

    size_t all_count = gw_zb_model_list_all_endpoints(all_eps, GW_ZB_MAX_ENDPOINTS);
    if (all_count == 0) {
        free(all_eps);
        return;
    }

    gw_device_t *devices = (gw_device_t *)calloc(GW_DEVICE_MAX_DEVICES, sizeof(gw_device_t));
    if (!devices) {
        ESP_LOGW(TAG, "snapshot refresh: no mem for device view");
        free(all_eps);
        return;
    }
    size_t dev_count = build_live_device_view(all_eps, all_count, devices, GW_DEVICE_MAX_DEVICES);
    uint32_t req_count = 0;
    for (size_t i = 0; i < dev_count; i++) {
        for (size_t ei = 0; ei < all_count; ei++) {
            if (!uid_equals_str(all_eps[ei].uid.uid, devices[i].device_uid.uid)) {
                continue;
            }
            queue_read_attr_if_cluster(&devices[i].device_uid, &all_eps[ei], 0x0006, 0x0000, &req_count); // onoff
            queue_read_attr_if_cluster(&devices[i].device_uid, &all_eps[ei], 0x0008, 0x0000, &req_count); // level
            queue_read_attr_if_cluster(&devices[i].device_uid, &all_eps[ei], 0x0300, 0x0003, &req_count); // color_x
            queue_read_attr_if_cluster(&devices[i].device_uid, &all_eps[ei], 0x0300, 0x0004, &req_count); // color_y
            queue_read_attr_if_cluster(&devices[i].device_uid, &all_eps[ei], 0x0300, 0x0007, &req_count); // color_temp
        }
    }

    if (req_count > 0) {
        TickType_t wait_ticks = pdMS_TO_TICKS(200 + (req_count * 20));
        TickType_t max_wait = pdMS_TO_TICKS(1500);
        if (wait_ticks > max_wait) {
            wait_ticks = max_wait;
        }
        ESP_LOGI(TAG, "snapshot refresh queued %u read_attr, wait %u ms", (unsigned)req_count,
                 (unsigned)(wait_ticks * portTICK_PERIOD_MS));
        vTaskDelay(wait_ticks);
    }

    free(devices);
    free(all_eps);
}

static bool endpoint_has_in_cluster(const gw_zb_endpoint_t *ep, uint16_t cluster_id)
{
    if (!ep || cluster_id == 0) {
        return false;
    }
    size_t n = ep->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : ep->in_cluster_count;
    for (size_t i = 0; i < n; i++) {
        if (ep->in_clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static uint8_t resolve_endpoint_for_cluster(const gw_device_uid_t *uid, uint16_t cluster_id)
{
    if (!uid || uid->uid[0] == '\0') {
        return 0;
    }
    gw_zb_endpoint_t *eps = (gw_zb_endpoint_t *)calloc(GW_ZB_MAX_ENDPOINTS, sizeof(gw_zb_endpoint_t));
    if (!eps) {
        ESP_LOGW(TAG, "resolve ep: no mem for endpoints uid=%s", uid->uid);
        return 0;
    }
    size_t ep_count = gw_zb_model_list_endpoints(uid, eps, GW_ZB_MAX_ENDPOINTS);
    uint8_t selected = 0;
    for (size_t i = 0; i < ep_count; i++) {
        if (endpoint_has_in_cluster(&eps[i], cluster_id)) {
            selected = eps[i].endpoint;
            break;
        }
    }
    if (selected == 0 && ep_count > 0) {
        selected = eps[0].endpoint;
    }
    free(eps);
    return selected;
}

static void uart_send_snapshot_states_for_device(const gw_device_t *d, uint32_t *snapshot_seq, uint16_t frame_seq)
{
    if (!d || !snapshot_seq) {
        return;
    }

    // Primary source of endpoint-scoped state: sensor_store (cluster/attr/endpoint).
    gw_sensor_value_t *sensors = (gw_sensor_value_t *)calloc(GW_SENSOR_MAX_VALUES, sizeof(gw_sensor_value_t));
    if (!sensors) {
        ESP_LOGW(TAG, "snapshot: no mem for sensor items uid=%s", d->device_uid.uid);
        return;
    }
    size_t sensor_count = gw_sensor_store_list(&d->device_uid, sensors, GW_SENSOR_MAX_VALUES);
    for (size_t i = 0; i < sensor_count; i++) {
        gw_uart_snapshot_v1_t snap = {0};
        snap.kind = GW_UART_SNAPSHOT_STATE;
        snap.snapshot_seq = (*snapshot_seq)++;
        strlcpy(snap.device_uid, d->device_uid.uid, sizeof(snap.device_uid));
        snap.short_addr = sensors[i].short_addr ? sensors[i].short_addr : d->short_addr;
        snap.endpoint = sensors[i].endpoint;
        snap.state_cluster_id = sensors[i].cluster_id;
        snap.state_attr_id = sensors[i].attr_id;
        snap.state_ts_ms = sensors[i].ts_ms;

        switch (sensors[i].value_type) {
            case GW_SENSOR_VALUE_I32:
                snap.state_value_type = GW_UART_VALUE_I64;
                snap.state_value_i64 = (int64_t)sensors[i].value_i32;
                break;
            case GW_SENSOR_VALUE_U32:
                snap.state_value_type = GW_UART_VALUE_I64;
                snap.state_value_i64 = (int64_t)sensors[i].value_u32;
                break;
            default:
                snap.state_value_type = GW_UART_VALUE_NONE;
                break;
        }
        uart_send_snapshot_frame(&snap, frame_seq);
    }
    free(sensors);

    // Fallback for normalized states not present in sensor_store (for example onoff).
    gw_state_item_t onoff = {0};
    if (gw_state_store_get(&d->device_uid, "onoff", &onoff) == ESP_OK && onoff.value_type == GW_STATE_VALUE_BOOL) {
        gw_uart_snapshot_v1_t snap = {0};
        snap.kind = GW_UART_SNAPSHOT_STATE;
        snap.snapshot_seq = (*snapshot_seq)++;
        strlcpy(snap.device_uid, d->device_uid.uid, sizeof(snap.device_uid));
        snap.short_addr = d->short_addr;
        snap.endpoint = resolve_endpoint_for_cluster(&d->device_uid, 0x0006);
        snap.state_cluster_id = 0x0006;
        snap.state_attr_id = 0x0000;
        snap.state_value_type = GW_UART_VALUE_BOOL;
        snap.state_value_bool = onoff.value_bool ? 1 : 0;
        snap.state_ts_ms = onoff.ts_ms;
        uart_send_snapshot_frame(&snap, frame_seq);
    }
}

static void uart_send_snapshot_device_delta(const char *uid_str, uint16_t seq)
{
    if (!uid_str || uid_str[0] == '\0') {
        return;
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, uid_str, sizeof(uid.uid));
    gw_device_t d = {0};
    if (gw_device_registry_get(&uid, &d) != ESP_OK) {
        return;
    }

    uint32_t snap_seq = 0;
    gw_uart_snapshot_v1_t snap = {0};
    snap.kind = GW_UART_SNAPSHOT_DEVICE;
    snap.snapshot_seq = snap_seq++;
    strlcpy(snap.device_uid, d.device_uid.uid, sizeof(snap.device_uid));
    strlcpy(snap.name, d.name, sizeof(snap.name));
    snap.short_addr = d.short_addr;
    snap.last_seen_ms = d.last_seen_ms;
    snap.has_onoff = d.has_onoff ? 1 : 0;
    snap.has_button = d.has_button ? 1 : 0;
    uart_send_snapshot_frame(&snap, seq);

    gw_zb_endpoint_t *eps = (gw_zb_endpoint_t *)calloc(16, sizeof(gw_zb_endpoint_t));
    if (!eps) {
        ESP_LOGW(TAG, "snapshot delta: no mem for endpoints");
        return;
    }
    size_t ep_count = gw_device_registry_list_endpoints(&d.device_uid, eps, 16);
    for (size_t i = 0; i < ep_count; i++) {
        memset(&snap, 0, sizeof(snap));
        snap.kind = GW_UART_SNAPSHOT_ENDPOINT;
        snap.snapshot_seq = snap_seq++;
        strlcpy(snap.device_uid, d.device_uid.uid, sizeof(snap.device_uid));
        snap.short_addr = eps[i].short_addr;
        snap.endpoint = eps[i].endpoint;
        snap.profile_id = eps[i].profile_id;
        snap.device_id = eps[i].device_id;
        snap.in_cluster_count = eps[i].in_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS ?
                                GW_UART_SNAPSHOT_MAX_CLUSTERS : eps[i].in_cluster_count;
        snap.out_cluster_count = eps[i].out_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS ?
                                 GW_UART_SNAPSHOT_MAX_CLUSTERS : eps[i].out_cluster_count;
        if (snap.in_cluster_count > 0) {
            memcpy(snap.in_clusters, eps[i].in_clusters, snap.in_cluster_count * sizeof(uint16_t));
        }
        if (snap.out_cluster_count > 0) {
            memcpy(snap.out_clusters, eps[i].out_clusters, snap.out_cluster_count * sizeof(uint16_t));
        }
        uart_send_snapshot_frame(&snap, seq);
    }
    free(eps);
    uart_send_snapshot_states_for_device(&d, &snap_seq, seq);
}

static esp_err_t uart_send_snapshot(uint16_t base_seq)
{
    gw_zb_endpoint_t *all_eps = (gw_zb_endpoint_t *)calloc(GW_ZB_MAX_ENDPOINTS, sizeof(gw_zb_endpoint_t));
    if (!all_eps) {
        return ESP_ERR_NO_MEM;
    }
    size_t all_count = gw_zb_model_list_all_endpoints(all_eps, GW_ZB_MAX_ENDPOINTS);
    gw_device_t *devices = (gw_device_t *)calloc(GW_DEVICE_MAX_DEVICES, sizeof(gw_device_t));
    if (!devices) {
        free(all_eps);
        return ESP_ERR_NO_MEM;
    }
    size_t dev_count = build_live_device_view(all_eps, all_count, devices, GW_DEVICE_MAX_DEVICES);
    uint32_t snap_seq = 0;

    gw_uart_snapshot_v1_t snap = {0};
    snap.kind = GW_UART_SNAPSHOT_BEGIN;
    snap.total_devices = (uint16_t)dev_count;
    snap.snapshot_seq = snap_seq++;
    uart_send_snapshot_frame(&snap, base_seq);

    for (size_t di = 0; di < dev_count; di++) {
        memset(&snap, 0, sizeof(snap));
        snap.kind = GW_UART_SNAPSHOT_DEVICE;
        snap.snapshot_seq = snap_seq++;
        strlcpy(snap.device_uid, devices[di].device_uid.uid, sizeof(snap.device_uid));
        strlcpy(snap.name, devices[di].name, sizeof(snap.name));
        snap.short_addr = devices[di].short_addr;
        snap.last_seen_ms = devices[di].last_seen_ms;
        snap.has_onoff = devices[di].has_onoff ? 1 : 0;
        snap.has_button = devices[di].has_button ? 1 : 0;
        uart_send_snapshot_frame(&snap, base_seq);

        for (size_t ei = 0; ei < all_count; ei++) {
            if (!uid_equals_str(all_eps[ei].uid.uid, devices[di].device_uid.uid)) {
                continue;
            }
            memset(&snap, 0, sizeof(snap));
            snap.kind = GW_UART_SNAPSHOT_ENDPOINT;
            snap.snapshot_seq = snap_seq++;
            strlcpy(snap.device_uid, devices[di].device_uid.uid, sizeof(snap.device_uid));
            snap.short_addr = all_eps[ei].short_addr;
            snap.endpoint = all_eps[ei].endpoint;
            snap.profile_id = all_eps[ei].profile_id;
            snap.device_id = all_eps[ei].device_id;
            snap.in_cluster_count = all_eps[ei].in_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS ?
                                    GW_UART_SNAPSHOT_MAX_CLUSTERS : all_eps[ei].in_cluster_count;
            snap.out_cluster_count = all_eps[ei].out_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS ?
                                     GW_UART_SNAPSHOT_MAX_CLUSTERS : all_eps[ei].out_cluster_count;
            if (snap.in_cluster_count > 0) {
                memcpy(snap.in_clusters, all_eps[ei].in_clusters, snap.in_cluster_count * sizeof(uint16_t));
            }
            if (snap.out_cluster_count > 0) {
                memcpy(snap.out_clusters, all_eps[ei].out_clusters, snap.out_cluster_count * sizeof(uint16_t));
            }
            uart_send_snapshot_frame(&snap, base_seq);
        }
        uart_send_snapshot_states_for_device(&devices[di], &snap_seq, base_seq);
    }

    memset(&snap, 0, sizeof(snap));
    snap.kind = GW_UART_SNAPSHOT_END;
    snap.total_devices = (uint16_t)dev_count;
    snap.snapshot_seq = snap_seq++;
    uart_send_snapshot_frame(&snap, base_seq);
    ESP_LOGI(TAG, "Snapshot sent: devices=%u frames=%u", (unsigned)dev_count, (unsigned)snap_seq);
    free(devices);
    free(all_eps);
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
    gw_uart_proto_frame_t frame = {
        .ver = GW_UART_PROTO_VERSION_V1,
        .msg_type = msg_type,
        .flags = 0,
        .seq = seq,
        .payload_len = payload_len,
    };
    uint8_t raw[GW_UART_PROTO_MAX_FRAME_SIZE];
    size_t raw_len = 0;

    if (payload_len > GW_UART_PROTO_MAX_PAYLOAD) {
        return;
    }
    if (payload_len > 0 && payload) {
        memcpy(frame.payload, payload, payload_len);
    }

    if (gw_uart_proto_build_frame(&frame, raw, sizeof(raw), &raw_len) != ESP_OK) {
        return;
    }
    if (msg_type == GW_UART_MSG_EVT) {
        ESP_LOGD(TAG, "UART TX %s seq=%u payload=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)payload_len);
    } else {
        ESP_LOGI(TAG, "UART TX %s seq=%u payload=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)payload_len);
    }
    if (!uart_write_all(raw, raw_len)) {
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
    if (strcmp(event->type, "device.join") == 0 && event->device_uid[0] != '\0') {
        uart_send_snapshot_device_delta(event->device_uid, s_evt_seq++);
    }
    if (strcmp(event->type, "device.leave") == 0 && event->device_uid[0] != '\0') {
        gw_uart_snapshot_v1_t snap = {0};
        snap.kind = GW_UART_SNAPSHOT_REMOVE;
        snap.snapshot_seq = s_evt_seq;
        strlcpy(snap.device_uid, event->device_uid, sizeof(snap.device_uid));
        snap.short_addr = event->short_addr;
        uart_send_snapshot_frame(&snap, s_evt_seq++);
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
            if (!has_uid || req->endpoint == 0 || req->cluster_id == 0 || req->attr_id == 0) {
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

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static void handle_cmd_req(const gw_uart_proto_frame_t *frame)
{
    gw_uart_cmd_req_v1_t req;
    memset(&req, 0, sizeof(req));

    size_t copy_len = frame->payload_len < sizeof(req) ? frame->payload_len : sizeof(req);
    if (copy_len > 0) {
        memcpy(&req, frame->payload, copy_len);
    }
    req.device_uid[sizeof(req.device_uid) - 1] = '\0';
    req.value_text[sizeof(req.value_text) - 1] = '\0';

    uint32_t req_id = req.req_id ? req.req_id : frame->seq;

    if ((gw_uart_cmd_id_t)req.cmd_id == GW_UART_CMD_SYNC_SNAPSHOT) {
        ESP_LOGI(TAG, "SYNC_SNAPSHOT requested (seq=%u req_id=%u)", (unsigned)frame->seq, (unsigned)req_id);
        // Сначала быстрый ACK для S3 (чтобы не ловить timeout), затем поток snapshot.
        uart_send_cmd_rsp(frame->seq, req_id, GW_UART_STATUS_OK, ESP_OK);
        snapshot_request_async();
        return;
    }

    esp_err_t err = exec_cmd_req(&req);
    gw_uart_status_t st = map_esp_err_to_status(err);
    uart_send_cmd_rsp(frame->seq, req_id, st, err);
}

static void handle_rx_frame(const gw_uart_proto_frame_t *frame)
{
    if (frame->msg_type == GW_UART_MSG_EVT) {
        ESP_LOGD(TAG, "UART RX %s seq=%u payload=%u", msg_type_name(frame->msg_type), (unsigned)frame->seq, (unsigned)frame->payload_len);
    } else {
        ESP_LOGI(TAG, "UART RX %s seq=%u payload=%u", msg_type_name(frame->msg_type), (unsigned)frame->seq, (unsigned)frame->payload_len);
    }

    switch ((gw_uart_msg_type_t)frame->msg_type) {
        case GW_UART_MSG_PING:
            uart_send_frame(GW_UART_MSG_PONG, frame->seq, NULL, 0);
            break;
        case GW_UART_MSG_HELLO:
            uart_send_frame(GW_UART_MSG_HELLO_ACK, frame->seq, NULL, 0);
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
        s_snapshot_requested = false;
        uart_refresh_states_before_snapshot();
        (void)uart_send_snapshot((uint16_t)(s_evt_seq++));

        // If requests arrived while we were streaming, loop once more immediately.
        while (s_snapshot_requested) {
            s_snapshot_requested = false;
            uart_refresh_states_before_snapshot();
            (void)uart_send_snapshot((uint16_t)(s_evt_seq++));
        }
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    gw_uart_proto_parser_t parser;
    gw_uart_proto_parser_init(&parser);

    for (;;) {
        int n = uart_read_bytes(GW_UART_PORT, rx, sizeof(rx), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        size_t off = 0;
        while (off < (size_t)n) {
            gw_uart_proto_frame_t frame;
            bool ready = false;
            size_t consumed = 0;
            esp_err_t err = gw_uart_proto_parser_feed(&parser, &rx[off], (size_t)n - off, &frame, &ready, &consumed);
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





