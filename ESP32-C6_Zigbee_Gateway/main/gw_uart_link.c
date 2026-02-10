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
#include "gw_core/gw_uart_proto.h"
#include "gw_core/types.h"
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
static volatile bool s_device_fb_requested;
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
static bool endpoint_has_in_cluster(const gw_zb_endpoint_t *ep, uint16_t cluster_id);
static void snapshot_request_async(void);
static void device_fb_request_async(void);

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
        case GW_UART_MSG_DEVICE_FB:
            return "DEVICE_FB";
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

static void uart_request_states_after_snapshot(void)
{
    gw_device_t *devices = (gw_device_t *)calloc(GW_DEVICE_MAX_DEVICES, sizeof(gw_device_t));
    if (!devices) {
        ESP_LOGW(TAG, "state sync: no mem for device list");
        return;
    }
    size_t dev_count = gw_device_registry_list(devices, GW_DEVICE_MAX_DEVICES);
    if (dev_count == 0) {
        free(devices);
        return;
    }

    gw_zb_endpoint_t *eps = (gw_zb_endpoint_t *)calloc(GW_DEVICE_MAX_ENDPOINTS, sizeof(gw_zb_endpoint_t));
    if (!eps) {
        ESP_LOGW(TAG, "state sync: no mem for endpoint list");
        free(devices);
        return;
    }
    uint32_t req_count = 0;
    for (size_t i = 0; i < dev_count; i++) {
        size_t ep_count = gw_device_registry_list_endpoints(&devices[i].device_uid, eps, GW_DEVICE_MAX_ENDPOINTS);
        for (size_t ei = 0; ei < ep_count; ei++) {
            queue_read_attr_if_cluster(&devices[i].device_uid, &eps[ei], 0x0006, 0x0000, &req_count); // onoff
            queue_read_attr_if_cluster(&devices[i].device_uid, &eps[ei], 0x0008, 0x0000, &req_count); // level
            queue_read_attr_if_cluster(&devices[i].device_uid, &eps[ei], 0x0300, 0x0003, &req_count); // color_x
            queue_read_attr_if_cluster(&devices[i].device_uid, &eps[ei], 0x0300, 0x0004, &req_count); // color_y
            queue_read_attr_if_cluster(&devices[i].device_uid, &eps[ei], 0x0300, 0x0007, &req_count); // color_temp
        }
    }

    ESP_LOGI(TAG, "state sync queued %u read_attr requests", (unsigned)req_count);
    free(eps);
    free(devices);
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

// Snapshot carries topology only (device + endpoint).
// Live state is synced separately via real Zigbee attr_report/read_attr pipeline.

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
}

static esp_err_t uart_send_snapshot(uint16_t base_seq)
{
    gw_device_t *devices = (gw_device_t *)calloc(GW_DEVICE_MAX_DEVICES, sizeof(gw_device_t));
    if (!devices) {
        return ESP_ERR_NO_MEM;
    }
    gw_zb_endpoint_t *eps = (gw_zb_endpoint_t *)calloc(GW_DEVICE_MAX_ENDPOINTS, sizeof(gw_zb_endpoint_t));
    if (!eps) {
        free(devices);
        return ESP_ERR_NO_MEM;
    }
    size_t dev_count = gw_device_registry_list(devices, GW_DEVICE_MAX_DEVICES);
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

        size_t ep_count = gw_device_registry_list_endpoints(&devices[di].device_uid, eps, GW_DEVICE_MAX_ENDPOINTS);
        for (size_t ei = 0; ei < ep_count; ei++) {
            memset(&snap, 0, sizeof(snap));
            snap.kind = GW_UART_SNAPSHOT_ENDPOINT;
            snap.snapshot_seq = snap_seq++;
            strlcpy(snap.device_uid, devices[di].device_uid.uid, sizeof(snap.device_uid));
            snap.short_addr = eps[ei].short_addr;
            snap.endpoint = eps[ei].endpoint;
            snap.profile_id = eps[ei].profile_id;
            snap.device_id = eps[ei].device_id;
            snap.in_cluster_count = eps[ei].in_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS ?
                                    GW_UART_SNAPSHOT_MAX_CLUSTERS : eps[ei].in_cluster_count;
            snap.out_cluster_count = eps[ei].out_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS ?
                                     GW_UART_SNAPSHOT_MAX_CLUSTERS : eps[ei].out_cluster_count;
            if (snap.in_cluster_count > 0) {
                memcpy(snap.in_clusters, eps[ei].in_clusters, snap.in_cluster_count * sizeof(uint16_t));
            }
            if (snap.out_cluster_count > 0) {
                memcpy(snap.out_clusters, eps[ei].out_clusters, snap.out_cluster_count * sizeof(uint16_t));
            }
            uart_send_snapshot_frame(&snap, base_seq);
        }
    }

    memset(&snap, 0, sizeof(snap));
    snap.kind = GW_UART_SNAPSHOT_END;
    snap.total_devices = (uint16_t)dev_count;
    snap.snapshot_seq = snap_seq++;
    uart_send_snapshot_frame(&snap, base_seq);
    ESP_LOGI(TAG, "Snapshot sent: devices=%u frames=%u", (unsigned)dev_count, (unsigned)snap_seq);
    free(eps);
    free(devices);
    return ESP_OK;
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t device_count;
    uint16_t endpoint_count;
    uint16_t reserved;
} __attribute__((packed)) gw_device_blob_hdr_t;

typedef struct {
    char device_uid[GW_DEVICE_UID_STRLEN];
    uint16_t short_addr;
    uint64_t last_seen_ms;
    uint8_t has_onoff;
    uint8_t has_button;
    char name[32];
} __attribute__((packed)) gw_device_blob_device_t;

typedef struct {
    char device_uid[GW_DEVICE_UID_STRLEN];
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t in_cluster_count;
    uint8_t out_cluster_count;
    uint16_t in_clusters[GW_UART_SNAPSHOT_MAX_CLUSTERS];
    uint16_t out_clusters[GW_UART_SNAPSHOT_MAX_CLUSTERS];
} __attribute__((packed)) gw_device_blob_endpoint_t;

static esp_err_t build_device_blob(uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    gw_device_t *devices = (gw_device_t *)calloc(GW_DEVICE_MAX_DEVICES, sizeof(gw_device_t));
    if (!devices) {
        return ESP_ERR_NO_MEM;
    }
    gw_zb_endpoint_t *eps = (gw_zb_endpoint_t *)calloc(GW_DEVICE_MAX_ENDPOINTS, sizeof(gw_zb_endpoint_t));
    if (!eps) {
        free(devices);
        return ESP_ERR_NO_MEM;
    }
    size_t dev_count = gw_device_registry_list(devices, GW_DEVICE_MAX_DEVICES);
    size_t endpoint_count = 0;
    for (size_t i = 0; i < dev_count; i++) {
        endpoint_count += gw_device_registry_list_endpoints(&devices[i].device_uid, eps, GW_DEVICE_MAX_ENDPOINTS);
    }

    size_t total = sizeof(gw_device_blob_hdr_t) +
                   dev_count * sizeof(gw_device_blob_device_t) +
                   endpoint_count * sizeof(gw_device_blob_endpoint_t);
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        free(eps);
        free(devices);
        return ESP_ERR_NO_MEM;
    }

    gw_device_blob_hdr_t hdr = {
        .magic = 0x31424644u, // "DFB1"
        .version = 1,
        .device_count = (uint16_t)dev_count,
        .endpoint_count = (uint16_t)endpoint_count,
        .reserved = 0,
    };
    size_t off = 0;
    memcpy(&buf[off], &hdr, sizeof(hdr));
    off += sizeof(hdr);

    for (size_t i = 0; i < dev_count; i++) {
        gw_device_blob_device_t d = {0};
        strlcpy(d.device_uid, devices[i].device_uid.uid, sizeof(d.device_uid));
        d.short_addr = devices[i].short_addr;
        d.last_seen_ms = devices[i].last_seen_ms;
        d.has_onoff = devices[i].has_onoff ? 1 : 0;
        d.has_button = devices[i].has_button ? 1 : 0;
        strlcpy(d.name, devices[i].name, sizeof(d.name));
        memcpy(&buf[off], &d, sizeof(d));
        off += sizeof(d);
    }

    for (size_t i = 0; i < dev_count; i++) {
        size_t ep_count = gw_device_registry_list_endpoints(&devices[i].device_uid, eps, GW_DEVICE_MAX_ENDPOINTS);
        for (size_t ei = 0; ei < ep_count; ei++) {
            gw_device_blob_endpoint_t ep = {0};
            strlcpy(ep.device_uid, eps[ei].uid.uid, sizeof(ep.device_uid));
            ep.short_addr = eps[ei].short_addr;
            ep.endpoint = eps[ei].endpoint;
            ep.profile_id = eps[ei].profile_id;
            ep.device_id = eps[ei].device_id;
            ep.in_cluster_count = eps[ei].in_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS
                                      ? GW_UART_SNAPSHOT_MAX_CLUSTERS
                                      : eps[ei].in_cluster_count;
            ep.out_cluster_count = eps[ei].out_cluster_count > GW_UART_SNAPSHOT_MAX_CLUSTERS
                                       ? GW_UART_SNAPSHOT_MAX_CLUSTERS
                                       : eps[ei].out_cluster_count;
            if (ep.in_cluster_count > 0) {
                memcpy(ep.in_clusters, eps[ei].in_clusters, ep.in_cluster_count * sizeof(uint16_t));
            }
            if (ep.out_cluster_count > 0) {
                memcpy(ep.out_clusters, eps[ei].out_clusters, ep.out_cluster_count * sizeof(uint16_t));
            }
            memcpy(&buf[off], &ep, sizeof(ep));
            off += sizeof(ep);
        }
    }

    free(eps);
    free(devices);
    *out_buf = buf;
    *out_len = off;
    return ESP_OK;
}

static void uart_send_device_fb_blob(uint16_t frame_seq)
{
    const size_t chunk_data_max = 180;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (build_device_blob(&blob, &blob_len) != ESP_OK || !blob || blob_len == 0) {
        ESP_LOGW(TAG, "device_fb build failed");
        free(blob);
        return;
    }

    uint16_t transfer_id = frame_seq;
    size_t offset = 0;
    while (offset < blob_len) {
        gw_uart_device_fb_chunk_v1_t ch = {0};
        ch.transfer_id = transfer_id;
        ch.total_len = (uint32_t)blob_len;
        ch.offset = (uint32_t)offset;
        size_t remain = blob_len - offset;
        size_t take = remain > chunk_data_max ? chunk_data_max : remain;
        ch.chunk_len = (uint8_t)take;
        if (offset == 0) {
            ch.flags |= GW_UART_DEVICE_FB_FLAG_BEGIN;
        }
        if (offset + take >= blob_len) {
            ch.flags |= GW_UART_DEVICE_FB_FLAG_END;
        }
        memcpy(ch.data, &blob[offset], take);
        uart_send_frame(GW_UART_MSG_DEVICE_FB, frame_seq, &ch, sizeof(ch));
        offset += take;
    }

    ESP_LOGI(TAG, "device_fb sent: bytes=%u chunks=%u", (unsigned)blob_len,
             (unsigned)((blob_len + chunk_data_max - 1) / chunk_data_max));
    free(blob);
}

static void snapshot_request_async(void)
{
    s_snapshot_requested = true;
    if (s_snapshot_task) {
        xTaskNotifyGive(s_snapshot_task);
    }
}

static void device_fb_request_async(void)
{
    s_device_fb_requested = true;
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
    if (strcmp(event->type, "zigbee_simple_desc") == 0) {
        // Endpoint topology was updated from live Zigbee model into storage.
        // Push refreshed blob so S3/UI see new endpoint metadata.
        device_fb_request_async();
    } else if (strcmp(event->type, "api_device_removed") == 0) {
        // Local remove on C6 should always propagate full metadata update to S3.
        device_fb_request_async();
    }
    if (!is_forwardable_event(event->type)) {
        return;
    }
    // During snapshot/device_fb transfer keep UART line dedicated to stream frames.
    if (s_snapshot_tx_active) {
        return;
    }
    if (strcmp(event->type, "device.join") == 0 && event->device_uid[0] != '\0') {
        uart_send_snapshot_device_delta(event->device_uid, s_evt_seq++);
        device_fb_request_async();
    }
    if (strcmp(event->type, "device.leave") == 0 && event->device_uid[0] != '\0') {
        gw_uart_snapshot_v1_t snap = {0};
        snap.kind = GW_UART_SNAPSHOT_REMOVE;
        snap.snapshot_seq = s_evt_seq;
        strlcpy(snap.device_uid, event->device_uid, sizeof(snap.device_uid));
        snap.short_addr = event->short_addr;
        uart_send_snapshot_frame(&snap, s_evt_seq++);
        device_fb_request_async();
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
        case GW_UART_CMD_SYNC_DEVICE_FB:
            device_fb_request_async();
            return ESP_OK;
        case GW_UART_CMD_SET_DEVICE_NAME: {
            if (!has_uid) {
                return ESP_ERR_INVALID_ARG;
            }
            char name_buf[sizeof(req->value_text)] = {0};
            strlcpy(name_buf, req->value_text, sizeof(name_buf));
            esp_err_t err = gw_device_registry_set_name(&uid, name_buf);
            if (err == ESP_OK) {
                device_fb_request_async();
            }
            return err;
        }

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
    if ((gw_uart_cmd_id_t)req.cmd_id == GW_UART_CMD_SYNC_SNAPSHOT ||
        (gw_uart_cmd_id_t)req.cmd_id == GW_UART_CMD_SYNC_DEVICE_FB) {
        ESP_LOGI(TAG,
                 "%s requested (seq=%u req_id=%u)",
                 ((gw_uart_cmd_id_t)req.cmd_id == GW_UART_CMD_SYNC_SNAPSHOT) ? "SYNC_SNAPSHOT" : "SYNC_DEVICE_FB",
                 (unsigned)frame->seq,
                 (unsigned)req_id);
        uart_send_cmd_rsp(frame->seq, req_id, GW_UART_STATUS_OK, ESP_OK);
        if ((gw_uart_cmd_id_t)req.cmd_id == GW_UART_CMD_SYNC_SNAPSHOT) {
            snapshot_request_async();
        } else {
            device_fb_request_async();
        }
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
        if (!s_snapshot_requested && !s_device_fb_requested) {
            continue;
        }
        if (s_snapshot_requested) {
            s_snapshot_requested = false;
            s_snapshot_tx_active = true;
            (void)uart_send_snapshot((uint16_t)(s_evt_seq++));
            if (s_device_fb_requested) {
                s_device_fb_requested = false;
                uart_send_device_fb_blob((uint16_t)(s_evt_seq++));
            }
            s_snapshot_tx_active = false;
            uart_request_states_after_snapshot();
        }
        if (s_device_fb_requested) {
            s_device_fb_requested = false;
            s_snapshot_tx_active = true;
            uart_send_device_fb_blob((uint16_t)(s_evt_seq++));
            s_snapshot_tx_active = false;
        }

        while (s_snapshot_requested || s_device_fb_requested) {
            if (s_snapshot_requested) {
                s_snapshot_requested = false;
                s_snapshot_tx_active = true;
                (void)uart_send_snapshot((uint16_t)(s_evt_seq++));
                if (s_device_fb_requested) {
                    s_device_fb_requested = false;
                    uart_send_device_fb_blob((uint16_t)(s_evt_seq++));
                }
                s_snapshot_tx_active = false;
                uart_request_states_after_snapshot();
            }
            if (s_device_fb_requested) {
                s_device_fb_requested = false;
                s_snapshot_tx_active = true;
                uart_send_device_fb_blob((uint16_t)(s_evt_seq++));
                s_snapshot_tx_active = false;
            }
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

    device_fb_request_async();
    ESP_LOGI(TAG, "UART binary link started: UART1 TX=GPIO%d RX=GPIO%d baud=%d", GW_UART_TX_PIN, GW_UART_RX_PIN, GW_UART_BAUD);
    return ESP_OK;
}





