#include "gw_zigbee/gw_zigbee.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "gw_core/event_bus.h"
#include "gw_core/gw_uart_proto.h"
#include "gw_core/device_fb_store.h"
#include "gw_core/runtime_sync.h"

static const char *TAG = "gw_zigbee_uart";

#if CONFIG_GW_ZIGBEE_UART_TRACE
#define GW_UART_TRACE_I(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define GW_UART_TRACE_D(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#else
#define GW_UART_TRACE_I(fmt, ...) do { (void)0; } while (0)
#define GW_UART_TRACE_D(fmt, ...) do { (void)0; } while (0)
#endif

#define GW_UART_PORT           ((uart_port_t)CONFIG_GW_ZIGBEE_UART_PORT)
#define GW_UART_TX_PIN         CONFIG_GW_ZIGBEE_UART_TX_PIN
#define GW_UART_RX_PIN         CONFIG_GW_ZIGBEE_UART_RX_PIN
#define GW_UART_BAUD           CONFIG_GW_ZIGBEE_UART_BAUD
#if CONFIG_GW_ZIGBEE_UART_RSP_TIMEOUT_MS < 2500
#define GW_UART_RESP_TIMEOUTMS 2500
#else
#define GW_UART_RESP_TIMEOUTMS CONFIG_GW_ZIGBEE_UART_RSP_TIMEOUT_MS
#endif
#define GW_UART_RX_BUF_SIZE    2048
#define GW_UART_TX_BUF_SIZE    2048
#define GW_UART_EVT_Q_LEN      8
#define GW_UART_RX_TASK_STACK  8192
#define GW_SNAPSHOT_IDLE_TIMEOUT_US  (3000000LL)
#define GW_SNAPSHOT_RETRY_GAP_US     (1000000LL)
#define GW_SNAPSHOT_RETRY_MAX        6
#define GW_DEVICE_FB_IDLE_TIMEOUT_US (3000000LL)
#define GW_DEVICE_FB_RETRY_GAP_US    (1000000LL)
#define GW_DEVICE_FB_RETRY_MAX       6

static TaskHandle_t s_rx_task;
static SemaphoreHandle_t s_init_lock;
static SemaphoreHandle_t s_cmd_lock;
static SemaphoreHandle_t s_rsp_sem;
static SemaphoreHandle_t s_tx_lock;
static bool s_started;
static uint16_t s_seq;

static portMUX_TYPE s_wait_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_wait_active;
static uint16_t s_wait_seq;
static gw_uart_cmd_rsp_v1_t s_wait_rsp;
static bool s_snapshot_stream_active;
static int64_t s_snapshot_last_chunk_us;
static int64_t s_snapshot_last_retry_us;
static uint8_t s_snapshot_retry_count;
static uint16_t s_snapshot_expected_devices;
static uint16_t s_snapshot_received_devices;
static uint16_t s_device_fb_transfer_id;
static uint32_t s_device_fb_expected_len;
static size_t s_device_fb_received_len;
static uint8_t *s_device_fb_buf;
static bool s_device_fb_active;
static int64_t s_device_fb_last_chunk_us;
static int64_t s_device_fb_last_retry_us;
static uint8_t s_device_fb_retry_count;
static esp_err_t uart_send_frame(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len);
static esp_err_t send_cmd_wait_rsp(gw_uart_cmd_req_v1_t *req);
static esp_err_t request_snapshot_sync(void);
static esp_err_t request_device_fb_sync(void);
static esp_err_t request_sync_cmd_async(gw_uart_cmd_id_t cmd_id, const char *label);

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

static const char *evt_id_name(uint8_t evt_id)
{
    switch ((gw_uart_evt_id_t)evt_id) {
        case GW_UART_EVT_ATTR_REPORT:
            return "ATTR_REPORT";
        case GW_UART_EVT_COMMAND:
            return "COMMAND";
        case GW_UART_EVT_DEVICE_JOIN:
            return "DEVICE_JOIN";
        case GW_UART_EVT_DEVICE_LEAVE:
            return "DEVICE_LEAVE";
        case GW_UART_EVT_NET_STATE:
            return "NET_STATE";
        default:
            return "UNKNOWN";
    }
}

static const char *cmd_id_name(uint8_t cmd_id)
{
    switch ((gw_uart_cmd_id_t)cmd_id) {
        case GW_UART_CMD_ONOFF:
            return "ONOFF";
        case GW_UART_CMD_LEVEL:
            return "LEVEL";
        case GW_UART_CMD_COLOR_XY:
            return "COLOR_XY";
        case GW_UART_CMD_COLOR_TEMP:
            return "COLOR_TEMP";
        case GW_UART_CMD_PERMIT_JOIN:
            return "PERMIT_JOIN";
        case GW_UART_CMD_READ_ATTR:
            return "READ_ATTR";
        case GW_UART_CMD_WRITE_ATTR:
            return "WRITE_ATTR";
        case GW_UART_CMD_IDENTIFY:
            return "IDENTIFY";
        case GW_UART_CMD_SYNC_SNAPSHOT:
            return "SYNC_SNAPSHOT";
        case GW_UART_CMD_SYNC_DEVICE_FB:
            return "SYNC_DEVICE_FB";
        case GW_UART_CMD_SET_DEVICE_NAME:
            return "SET_DEVICE_NAME";
        default:
            return "UNKNOWN";
    }
}

static const char *cluster_name(uint16_t cluster_id)
{
    switch (cluster_id) {
        case 0x0006:
            return "OnOff";
        case 0x0008:
            return "Level";
        case 0x0300:
            return "ColorControl";
        case 0x0402:
            return "Temperature";
        case 0x0405:
            return "Humidity";
        default:
            return "-";
    }
}

static const char *onoff_param_name(int32_t p0)
{
    switch (p0) {
        case 0:
            return "off";
        case 1:
            return "on";
        case 2:
            return "toggle";
        default:
            return "?";
    }
}

static const char *status_name(uint16_t status)
{
    switch ((gw_uart_status_t)status) {
        case GW_UART_STATUS_OK:
            return "OK";
        case GW_UART_STATUS_INVALID_ARGS:
            return "INVALID_ARGS";
        case GW_UART_STATUS_NOT_READY:
            return "NOT_READY";
        case GW_UART_STATUS_NOT_FOUND:
            return "NOT_FOUND";
        case GW_UART_STATUS_UNSUPPORTED:
            return "UNSUPPORTED";
        case GW_UART_STATUS_BUSY:
            return "BUSY";
        case GW_UART_STATUS_TIMEOUT:
            return "TIMEOUT";
        case GW_UART_STATUS_INTERNAL_ERROR:
            return "INTERNAL_ERROR";
        case GW_UART_STATUS_TRANSPORT_CRC_ERROR:
            return "CRC_ERROR";
        case GW_UART_STATUS_TRANSPORT_FORMAT:
            return "FORMAT_ERROR";
        default:
            return "UNKNOWN";
    }
}

static esp_err_t map_status_to_err(uint16_t status)
{
    switch ((gw_uart_status_t)status) {
        case GW_UART_STATUS_OK:
            return ESP_OK;
        case GW_UART_STATUS_INVALID_ARGS:
            return ESP_ERR_INVALID_ARG;
        case GW_UART_STATUS_NOT_READY:
            return ESP_ERR_INVALID_STATE;
        case GW_UART_STATUS_NOT_FOUND:
            return ESP_ERR_NOT_FOUND;
        case GW_UART_STATUS_UNSUPPORTED:
            return ESP_ERR_NOT_SUPPORTED;
        case GW_UART_STATUS_BUSY:
            return ESP_ERR_NO_MEM;
        case GW_UART_STATUS_TIMEOUT:
            return ESP_ERR_TIMEOUT;
        default:
            return ESP_FAIL;
    }
}

static gw_event_value_type_t map_value_type(uint8_t t)
{
    switch ((gw_uart_value_type_t)t) {
        case GW_UART_VALUE_BOOL:
            return GW_EVENT_VALUE_BOOL;
        case GW_UART_VALUE_I64:
            return GW_EVENT_VALUE_I64;
        case GW_UART_VALUE_F32:
            return GW_EVENT_VALUE_F64;
        case GW_UART_VALUE_TEXT:
            return GW_EVENT_VALUE_TEXT;
        case GW_UART_VALUE_NONE:
        default:
            return GW_EVENT_VALUE_NONE;
    }
}

static const char *fallback_evt_type(uint8_t evt_id)
{
    switch ((gw_uart_evt_id_t)evt_id) {
        case GW_UART_EVT_ATTR_REPORT:
            return "zigbee.attr_report";
        case GW_UART_EVT_COMMAND:
            return "zigbee.command";
        case GW_UART_EVT_DEVICE_JOIN:
            return "device.join";
        case GW_UART_EVT_DEVICE_LEAVE:
            return "device.leave";
        case GW_UART_EVT_NET_STATE:
        default:
            return "zigbee.net_state";
    }
}

static void normalize_evt_type(const char *in_type, char *out_type, size_t out_size, uint8_t evt_id)
{
    if (!out_type || out_size == 0) {
        return;
    }
    out_type[0] = '\0';

    if (in_type && in_type[0]) {
        if (strncmp(in_type, "zigbee_", 7) == 0) {
            (void)snprintf(out_type, out_size, "zigbee.%s", in_type + 7);
            return;
        }
        strlcpy(out_type, in_type, out_size);
        return;
    }

    strlcpy(out_type, fallback_evt_type(evt_id), out_size);
}

static void publish_evt_from_c6(const gw_uart_evt_v1_t *evt)
{
    char type_buf[32];
    normalize_evt_type(evt->event_type, type_buf, sizeof(type_buf), evt->evt_id);
    gw_event_value_type_t vtype = map_value_type(evt->value_type);
    const char *cmd = evt->cmd;
    char cmd_fallback[16] = {0};
    if ((!cmd || cmd[0] == '\0') && evt->evt_id == GW_UART_EVT_COMMAND) {
        if (evt->cluster_id == 0x0006) {
            strlcpy(cmd_fallback, "toggle", sizeof(cmd_fallback));
            cmd = cmd_fallback;
        }
    }

    gw_event_bus_publish_zb(type_buf,
                            "zigbee-uart",
                            evt->device_uid,
                            evt->short_addr,
                            "from_c6",
                            evt->endpoint,
                            cmd,
                            evt->cluster_id,
                            evt->attr_id,
                            vtype,
                            evt->value_bool != 0,
                            evt->value_i64,
                            (double)evt->value_f32,
                            evt->value_text,
                            NULL,
                            0);
}

static void apply_snapshot_from_c6(const gw_uart_snapshot_v1_t *snap)
{
    if (!snap) {
        return;
    }
    s_snapshot_last_chunk_us = esp_timer_get_time();

    switch ((gw_uart_snapshot_kind_t)snap->kind) {
        case GW_UART_SNAPSHOT_BEGIN:
            s_snapshot_stream_active = true;
            s_snapshot_retry_count = 0;
            s_snapshot_last_retry_us = 0;
            s_snapshot_expected_devices = snap->total_devices;
            s_snapshot_received_devices = 0;
            (void)gw_runtime_sync_snapshot_begin(snap->total_devices);
            ESP_LOGI(TAG, "Snapshot begin: total_devices=%u", (unsigned)snap->total_devices);
            break;
        case GW_UART_SNAPSHOT_DEVICE: {
            gw_device_t d = {0};
            strlcpy(d.device_uid.uid, snap->device_uid, sizeof(d.device_uid.uid));
            strlcpy(d.name, snap->name, sizeof(d.name));
            d.short_addr = snap->short_addr;
            d.last_seen_ms = snap->last_seen_ms;
            d.has_onoff = (snap->has_onoff != 0);
            d.has_button = (snap->has_button != 0);
            (void)gw_runtime_sync_snapshot_upsert_device(&d);
            s_snapshot_received_devices++;
            break;
        }
        case GW_UART_SNAPSHOT_ENDPOINT: {
            gw_zb_endpoint_t ep = {0};
            strlcpy(ep.uid.uid, snap->device_uid, sizeof(ep.uid.uid));
            ep.short_addr = snap->short_addr;
            ep.endpoint = snap->endpoint;
            ep.profile_id = snap->profile_id;
            ep.device_id = snap->device_id;
            ep.in_cluster_count = snap->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : snap->in_cluster_count;
            ep.out_cluster_count = snap->out_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : snap->out_cluster_count;
            if (ep.in_cluster_count > 0) {
                memcpy(ep.in_clusters, snap->in_clusters, ep.in_cluster_count * sizeof(uint16_t));
            }
            if (ep.out_cluster_count > 0) {
                memcpy(ep.out_clusters, snap->out_clusters, ep.out_cluster_count * sizeof(uint16_t));
            }
            (void)gw_runtime_sync_snapshot_upsert_endpoint(&ep);
            break;
        }
        case GW_UART_SNAPSHOT_STATE: {
            gw_event_value_type_t vtype = map_value_type(snap->state_value_type);
            gw_event_bus_publish_zb("zigbee.attr_report",
                                    "zigbee-uart-snapshot",
                                    snap->device_uid,
                                    snap->short_addr,
                                    "snapshot state",
                                    snap->endpoint,
                                    NULL,
                                    snap->state_cluster_id,
                                    snap->state_attr_id,
                                    vtype,
                                    snap->state_value_bool != 0,
                                    snap->state_value_i64,
                                    (double)snap->state_value_f32,
                                    snap->state_value_text,
                                    NULL,
                                    0);
            break;
        }
        case GW_UART_SNAPSHOT_REMOVE: {
            gw_device_uid_t uid = {0};
            strlcpy(uid.uid, snap->device_uid, sizeof(uid.uid));
            (void)gw_runtime_sync_snapshot_remove_device(&uid);
            break;
        }
        case GW_UART_SNAPSHOT_END:
            (void)gw_runtime_sync_snapshot_end();
            ESP_LOGI(TAG, "Snapshot end: expected=%u received=%u",
                     (unsigned)s_snapshot_expected_devices, (unsigned)s_snapshot_received_devices);
            s_snapshot_stream_active = false;
            s_snapshot_last_chunk_us = 0;
            s_snapshot_last_retry_us = 0;
            s_snapshot_retry_count = 0;
            if (s_snapshot_expected_devices > 0 && s_snapshot_received_devices < s_snapshot_expected_devices) {
                ESP_LOGW(TAG, "Snapshot incomplete, requesting re-sync");
                (void)request_snapshot_sync();
            } else {
                // After a completed snapshot, always refresh the device flatbuffer.
                // This guarantees frontend store sync after any snapshot recovery cycle.
                (void)request_sync_cmd_async(GW_UART_CMD_SYNC_DEVICE_FB, "device fb sync");
            }
            break;
        default:
            break;
    }
}

static void apply_device_fb_chunk_from_c6(const gw_uart_device_fb_chunk_v1_t *ch)
{
    if (!ch || ch->chunk_len > sizeof(ch->data)) {
        return;
    }

    if (ch->flags & GW_UART_DEVICE_FB_FLAG_BEGIN) {
        free(s_device_fb_buf);
        s_device_fb_buf = NULL;
        s_device_fb_received_len = 0;
        s_device_fb_expected_len = ch->total_len;
        s_device_fb_transfer_id = ch->transfer_id;
        s_device_fb_active = false;
        s_device_fb_last_chunk_us = esp_timer_get_time();
        if (s_device_fb_expected_len > 0) {
            s_device_fb_buf = (uint8_t *)malloc(s_device_fb_expected_len);
            if (!s_device_fb_buf) {
                ESP_LOGW(TAG, "device fb alloc failed: %u bytes", (unsigned)s_device_fb_expected_len);
                s_device_fb_expected_len = 0;
            } else {
                s_device_fb_active = true;
                s_device_fb_retry_count = 0;
                ESP_LOGI(TAG, "device fb begin: transfer=%u total=%u", (unsigned)ch->transfer_id, (unsigned)ch->total_len);
            }
        }
    }

    if (!s_device_fb_buf || s_device_fb_expected_len == 0) {
        return;
    }
    if (ch->transfer_id != s_device_fb_transfer_id) {
        return;
    }
    if ((size_t)ch->offset + ch->chunk_len > s_device_fb_expected_len) {
        ESP_LOGW(TAG, "device fb chunk out of bounds");
        return;
    }

    s_device_fb_last_chunk_us = esp_timer_get_time();
    memcpy(&s_device_fb_buf[ch->offset], ch->data, ch->chunk_len);
    size_t end = (size_t)ch->offset + ch->chunk_len;
    if (end > s_device_fb_received_len) {
        s_device_fb_received_len = end;
    }
    if ((ch->flags & GW_UART_DEVICE_FB_FLAG_END) == 0) {
        ESP_LOGI(TAG, "device fb chunk: transfer=%u off=%u len=%u recv=%u/%u",
                 (unsigned)ch->transfer_id,
                 (unsigned)ch->offset,
                 (unsigned)ch->chunk_len,
                 (unsigned)s_device_fb_received_len,
                 (unsigned)s_device_fb_expected_len);
    }

    if (ch->flags & GW_UART_DEVICE_FB_FLAG_END) {
        if (s_device_fb_received_len == s_device_fb_expected_len) {
            (void)gw_device_fb_store_set(s_device_fb_buf, s_device_fb_expected_len);
            ESP_LOGI(TAG, "device fb updated: %u bytes", (unsigned)s_device_fb_expected_len);
        } else {
            ESP_LOGW(TAG,
                     "device fb incomplete: recv=%u expected=%u",
                     (unsigned)s_device_fb_received_len,
                     (unsigned)s_device_fb_expected_len);
        }
        free(s_device_fb_buf);
        s_device_fb_buf = NULL;
        s_device_fb_received_len = 0;
        s_device_fb_expected_len = 0;
        s_device_fb_transfer_id = 0;
        s_device_fb_active = false;
        s_device_fb_last_chunk_us = 0;
        s_device_fb_last_retry_us = 0;
        s_device_fb_retry_count = 0;
    }
}

static esp_err_t request_snapshot_sync(void)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_SYNC_SNAPSHOT;
    esp_err_t err = send_cmd_wait_rsp(&req);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "snapshot sync request failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "snapshot sync requested");
    }
    return err;
}

static esp_err_t request_device_fb_sync(void)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_SYNC_DEVICE_FB;
    esp_err_t err = send_cmd_wait_rsp(&req);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device fb sync request failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "device fb sync requested");
    }
    return err;
}

static esp_err_t request_sync_cmd_async(gw_uart_cmd_id_t cmd_id, const char *label)
{
    if (!label) {
        label = "sync";
    }
    if (!s_cmd_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_cmd_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = (uint8_t)cmd_id;
    uint16_t seq = ++s_seq;
    req.req_id = seq;
    esp_err_t err = uart_send_frame(GW_UART_MSG_CMD_REQ, seq, &req, sizeof(req));
    xSemaphoreGive(s_cmd_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s requested (async)", label);
    } else {
        ESP_LOGW(TAG, "%s async request failed: %s", label, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t uart_send_frame(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len)
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
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_len > 0 && payload) {
        memcpy(frame.payload, payload, payload_len);
    }

    ESP_RETURN_ON_ERROR(gw_uart_proto_build_frame(&frame, raw, sizeof(raw), &raw_len), TAG, "build_frame failed");
    if (msg_type == GW_UART_MSG_EVT) {
        GW_UART_TRACE_D("UART TX %s seq=%u payload=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)payload_len);
    } else {
        GW_UART_TRACE_I("UART TX %s seq=%u payload=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)payload_len);
    }
    if (s_tx_lock) {
        (void)xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    bool ok = uart_write_all(raw, raw_len);
    if (s_tx_lock) {
        (void)xSemaphoreGive(s_tx_lock);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static void handle_rx_frame(const gw_uart_proto_frame_t *frame)
{
    if (frame->msg_type == GW_UART_MSG_EVT) {
        GW_UART_TRACE_D("UART RX %s seq=%u payload=%u", msg_type_name(frame->msg_type), (unsigned)frame->seq, (unsigned)frame->payload_len);
    } else {
        GW_UART_TRACE_I("UART RX %s seq=%u payload=%u", msg_type_name(frame->msg_type), (unsigned)frame->seq, (unsigned)frame->payload_len);
    }

    if (frame->msg_type == GW_UART_MSG_EVT) {
        if (frame->payload_len == 0) {
            return;
        }
        gw_uart_evt_v1_t evt = {0};
        size_t n = frame->payload_len < sizeof(evt) ? frame->payload_len : sizeof(evt);
        memcpy(&evt, frame->payload, n);
        evt.event_type[sizeof(evt.event_type) - 1] = '\0';
        evt.cmd[sizeof(evt.cmd) - 1] = '\0';
        evt.device_uid[sizeof(evt.device_uid) - 1] = '\0';
        evt.value_text[sizeof(evt.value_text) - 1] = '\0';
        GW_UART_TRACE_I("UART EVT %s(%u) type=%s uid=%s short=0x%04x ep=%u cluster=0x%04x(%s) attr=0x%04x cmd=%s",
                        evt_id_name(evt.evt_id), (unsigned)evt.evt_id, evt.event_type,
                        evt.device_uid, (unsigned)evt.short_addr, (unsigned)evt.endpoint,
                        (unsigned)evt.cluster_id, cluster_name(evt.cluster_id), (unsigned)evt.attr_id, evt.cmd);
        publish_evt_from_c6(&evt);
        return;
    }

    if (frame->msg_type == GW_UART_MSG_CMD_RSP) {
        gw_uart_cmd_rsp_v1_t rsp = {0};
        size_t n = frame->payload_len < sizeof(rsp) ? frame->payload_len : sizeof(rsp);
        memcpy(&rsp, frame->payload, n);
        rsp.message[sizeof(rsp.message) - 1] = '\0';
        GW_UART_TRACE_I("UART RSP seq=%u status=%u(%s) msg=%s",
                        (unsigned)frame->seq, (unsigned)rsp.status, status_name(rsp.status), rsp.message);

        bool match = false;
        portENTER_CRITICAL(&s_wait_lock);
        if (s_wait_active && frame->seq == s_wait_seq) {
            s_wait_rsp = rsp;
            s_wait_active = false;
            match = true;
        }
        portEXIT_CRITICAL(&s_wait_lock);
        if (match) {
            xSemaphoreGive(s_rsp_sem);
        }
        return;
    }

    if (frame->msg_type == GW_UART_MSG_SNAPSHOT) {
        gw_uart_snapshot_v1_t snap = {0};
        size_t n = frame->payload_len < sizeof(snap) ? frame->payload_len : sizeof(snap);
        memcpy(&snap, frame->payload, n);
        snap.device_uid[sizeof(snap.device_uid) - 1] = '\0';
        snap.name[sizeof(snap.name) - 1] = '\0';
        snap.state_value_text[sizeof(snap.state_value_text) - 1] = '\0';
        GW_UART_TRACE_I("UART SNAP kind=%u seq=%u uid=%s ep=%u",
                        (unsigned)snap.kind, (unsigned)snap.snapshot_seq,
                        snap.device_uid, (unsigned)snap.endpoint);
        apply_snapshot_from_c6(&snap);
        return;
    }

    if (frame->msg_type == GW_UART_MSG_DEVICE_FB) {
        gw_uart_device_fb_chunk_v1_t ch = {0};
        size_t n = frame->payload_len < sizeof(ch) ? frame->payload_len : sizeof(ch);
        memcpy(&ch, frame->payload, n);
        apply_device_fb_chunk_from_c6(&ch);
        return;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    gw_uart_proto_parser_t parser;
    gw_uart_proto_parser_init(&parser);

    for (;;) {
        // Watch stalled streams regardless of incoming event traffic.
        int64_t now_us = esp_timer_get_time();
        if (s_snapshot_stream_active && s_snapshot_last_chunk_us > 0) {
            if ((now_us - s_snapshot_last_chunk_us) > GW_SNAPSHOT_IDLE_TIMEOUT_US &&
                (now_us - s_snapshot_last_retry_us) > GW_SNAPSHOT_RETRY_GAP_US &&
                s_snapshot_retry_count < GW_SNAPSHOT_RETRY_MAX) {
                s_snapshot_last_retry_us = now_us;
                s_snapshot_retry_count++;
                ESP_LOGW(TAG,
                         "snapshot stalled: received=%u/%u retry=%u/%u",
                         (unsigned)s_snapshot_received_devices,
                         (unsigned)s_snapshot_expected_devices,
                         (unsigned)s_snapshot_retry_count,
                         (unsigned)GW_SNAPSHOT_RETRY_MAX);
                (void)request_sync_cmd_async(GW_UART_CMD_SYNC_SNAPSHOT, "snapshot sync");
            }
        }
        if (s_device_fb_active && s_device_fb_expected_len > 0 && s_device_fb_last_chunk_us > 0) {
            if ((now_us - s_device_fb_last_chunk_us) > GW_DEVICE_FB_IDLE_TIMEOUT_US &&
                (now_us - s_device_fb_last_retry_us) > GW_DEVICE_FB_RETRY_GAP_US &&
                s_device_fb_retry_count < GW_DEVICE_FB_RETRY_MAX) {
                s_device_fb_last_retry_us = now_us;
                s_device_fb_retry_count++;
                ESP_LOGW(TAG,
                         "device fb stalled: recv=%u/%u transfer=%u retry=%u/%u",
                         (unsigned)s_device_fb_received_len,
                         (unsigned)s_device_fb_expected_len,
                         (unsigned)s_device_fb_transfer_id,
                         (unsigned)s_device_fb_retry_count,
                         (unsigned)GW_DEVICE_FB_RETRY_MAX);
                (void)request_sync_cmd_async(GW_UART_CMD_SYNC_DEVICE_FB, "device fb sync");
            }
        }

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
                ESP_LOGW(TAG, "UART parse error: %s", esp_err_to_name(err));
                continue;
            }
            if (ready) {
                handle_rx_frame(&frame);
            }
        }
    }
}

static esp_err_t ensure_started(void)
{
    if (!s_init_lock) {
        s_init_lock = xSemaphoreCreateMutex();
    }
    if (!s_cmd_lock) {
        s_cmd_lock = xSemaphoreCreateMutex();
    }
    if (!s_rsp_sem) {
        s_rsp_sem = xSemaphoreCreateBinary();
    }
    if (!s_tx_lock) {
        s_tx_lock = xSemaphoreCreateMutex();
    }
    if (!s_init_lock || !s_cmd_lock || !s_rsp_sem || !s_tx_lock) {
        ESP_LOGW(TAG,
                 "sync primitive alloc failed: internal=%u dma=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_init_lock, portMAX_DELAY);
    if (s_started) {
        xSemaphoreGive(s_init_lock);
        return ESP_OK;
    }

    const uart_config_t cfg = {
        .baud_rate = GW_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = GW_UART_SCLK_SRC,
    };

    bool driver_installed_here = false;
    esp_err_t err = uart_driver_install(GW_UART_PORT, GW_UART_RX_BUF_SIZE, GW_UART_TX_BUF_SIZE, GW_UART_EVT_Q_LEN, NULL, 0);
    if (err == ESP_OK) {
        driver_installed_here = true;
    } else if (uart_is_driver_installed(GW_UART_PORT)) {
        // Previous init attempt may have left the driver installed.
        ESP_LOGW(TAG, "UART driver already installed on port %d, reusing", (int)GW_UART_PORT);
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_init_lock);
        return err;
    }
    err = uart_param_config(GW_UART_PORT, &cfg);
    if (err != ESP_OK) {
        if (driver_installed_here) {
            (void)uart_driver_delete(GW_UART_PORT);
        }
        xSemaphoreGive(s_init_lock);
        return err;
    }
    err = uart_set_pin(GW_UART_PORT, GW_UART_TX_PIN, GW_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        if (driver_installed_here) {
            (void)uart_driver_delete(GW_UART_PORT);
        }
        xSemaphoreGive(s_init_lock);
        return err;
    }

    // Must run on internal stack: this task can touch NVS/flash paths during snapshot apply.
    if (xTaskCreate(rx_task, "zb_uart_rx", GW_UART_RX_TASK_STACK, NULL, 7, &s_rx_task) != pdPASS) {
        if (driver_installed_here) {
            (void)uart_driver_delete(GW_UART_PORT);
        }
        ESP_LOGW(TAG,
                 "rx task create failed: internal=%u dma=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        xSemaphoreGive(s_init_lock);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "C6 link UART started: port=%d TX=%d RX=%d baud=%d",
             (int)GW_UART_PORT, GW_UART_TX_PIN, GW_UART_RX_PIN, GW_UART_BAUD);

    xSemaphoreGive(s_init_lock);

    /* Нестрогий handshake: если C6 не ответит, рабочий режим команд все равно возможен. */
    (void)uart_send_frame(GW_UART_MSG_HELLO, ++s_seq, NULL, 0);
    (void)uart_send_frame(GW_UART_MSG_PING, ++s_seq, NULL, 0);
    return ESP_OK;
}

static esp_err_t send_cmd_wait_rsp(gw_uart_cmd_req_v1_t *req)
{
    ESP_RETURN_ON_ERROR(ensure_started(), TAG, "uart start failed");

    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(GW_UART_RESP_TIMEOUTMS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t seq = ++s_seq;
    req->req_id = seq;
    if ((gw_uart_cmd_id_t)req->cmd_id == GW_UART_CMD_ONOFF) {
        GW_UART_TRACE_I("UART CMD %s(%u) req_id=%u uid=%s ep=%u action=%s(%d)",
                        cmd_id_name(req->cmd_id), (unsigned)req->cmd_id, (unsigned)req->req_id,
                        req->device_uid, (unsigned)req->endpoint, onoff_param_name(req->param0), (int)req->param0);
    } else {
        GW_UART_TRACE_I("UART CMD %s(%u) req_id=%u uid=%s ep=%u cl=0x%04x(%s) attr=0x%04x p0=%d p1=%d p2=%d",
                        cmd_id_name(req->cmd_id), (unsigned)req->cmd_id, (unsigned)req->req_id,
                        req->device_uid, (unsigned)req->endpoint,
                        (unsigned)req->cluster_id, cluster_name(req->cluster_id), (unsigned)req->attr_id,
                        (int)req->param0, (int)req->param1, (int)req->param2);
    }

    while (xSemaphoreTake(s_rsp_sem, 0) == pdTRUE) {
    }

    portENTER_CRITICAL(&s_wait_lock);
    s_wait_seq = seq;
    s_wait_active = true;
    memset(&s_wait_rsp, 0, sizeof(s_wait_rsp));
    portEXIT_CRITICAL(&s_wait_lock);

    esp_err_t err = uart_send_frame(GW_UART_MSG_CMD_REQ, seq, req, sizeof(*req));
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_wait_lock);
        s_wait_active = false;
        portEXIT_CRITICAL(&s_wait_lock);
        xSemaphoreGive(s_cmd_lock);
        return err;
    }

    if (xSemaphoreTake(s_rsp_sem, pdMS_TO_TICKS(GW_UART_RESP_TIMEOUTMS)) != pdTRUE) {
        portENTER_CRITICAL(&s_wait_lock);
        s_wait_active = false;
        portEXIT_CRITICAL(&s_wait_lock);
        xSemaphoreGive(s_cmd_lock);
        return ESP_ERR_TIMEOUT;
    }

    gw_uart_cmd_rsp_v1_t rsp = {0};
    portENTER_CRITICAL(&s_wait_lock);
    rsp = s_wait_rsp;
    portEXIT_CRITICAL(&s_wait_lock);

    xSemaphoreGive(s_cmd_lock);
    return map_status_to_err(rsp.status);
}

static void fill_uid(char dst[19], const gw_device_uid_t *uid)
{
    if (!dst) {
        return;
    }
    dst[0] = '\0';
    if (!uid) {
        return;
    }
    strlcpy(dst, uid->uid, 19);
}

esp_err_t gw_zigbee_link_start(void)
{
    esp_err_t err = ensure_started();
    if (err != ESP_OK) {
        return err;
    }
    err = request_snapshot_sync();
    if (err != ESP_OK) {
        // Defer retry to the regular async recovery path.
        (void)request_sync_cmd_async(GW_UART_CMD_SYNC_SNAPSHOT, "snapshot sync");
        return ESP_OK;
    }
    (void)request_device_fb_sync();
    return ESP_OK;
}

esp_err_t gw_zigbee_sync_device_fb(void)
{
    esp_err_t err = ensure_started();
    if (err != ESP_OK) {
        return err;
    }
    return request_device_fb_sync();
}

esp_err_t gw_zigbee_set_device_name(const gw_device_uid_t *uid, const char *name)
{
    if (!uid || !uid->uid[0] || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_SET_DEVICE_NAME;
    fill_uid(req.device_uid, uid);
    strlcpy(req.value_text, name, sizeof(req.value_text));
    return send_cmd_wait_rsp(&req);
}

esp_err_t gw_zigbee_permit_join(uint8_t seconds)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_PERMIT_JOIN;
    req.param0 = seconds;
    return send_cmd_wait_rsp(&req);
}

void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability)
{
    (void)ieee_addr;
    (void)short_addr;
    (void)capability;
}

esp_err_t gw_zigbee_device_leave(const gw_device_uid_t *uid, uint16_t short_addr, bool rejoin)
{
    (void)uid;
    (void)short_addr;
    (void)rejoin;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_discover_by_short(uint16_t short_addr)
{
    (void)short_addr;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_onoff_cmd(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_onoff_cmd_t cmd)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_ONOFF;
    fill_uid(req.device_uid, uid);
    req.endpoint = endpoint;
    req.param0 = (int32_t)cmd;
    return send_cmd_wait_rsp(&req);
}

esp_err_t gw_zigbee_level_move_to_level(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_level_t level)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_LEVEL;
    fill_uid(req.device_uid, uid);
    req.endpoint = endpoint;
    req.param0 = level.level;
    req.param1 = (int32_t)(level.transition_ms / 100);
    return send_cmd_wait_rsp(&req);
}

esp_err_t gw_zigbee_color_move_to_xy(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_xy_t color)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_COLOR_XY;
    fill_uid(req.device_uid, uid);
    req.endpoint = endpoint;
    req.param0 = color.x;
    req.param1 = color.y;
    req.param2 = (int32_t)(color.transition_ms / 100);
    return send_cmd_wait_rsp(&req);
}

esp_err_t gw_zigbee_color_move_to_temp(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_temp_t temp)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_COLOR_TEMP;
    fill_uid(req.device_uid, uid);
    req.endpoint = endpoint;
    req.param0 = temp.mireds;
    req.param1 = (int32_t)(temp.transition_ms / 100);
    return send_cmd_wait_rsp(&req);
}

esp_err_t gw_zigbee_group_onoff_cmd(uint16_t group_id, gw_zigbee_onoff_cmd_t cmd)
{
    (void)group_id;
    (void)cmd;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_group_level_move_to_level(uint16_t group_id, gw_zigbee_level_t level)
{
    (void)group_id;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_group_color_move_to_xy(uint16_t group_id, gw_zigbee_color_xy_t color)
{
    (void)group_id;
    (void)color;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_group_color_move_to_temp(uint16_t group_id, gw_zigbee_color_temp_t temp)
{
    (void)group_id;
    (void)temp;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_read_onoff_state(const gw_device_uid_t *uid, uint8_t endpoint)
{
    return gw_zigbee_read_attr(uid, endpoint, 0x0006, 0x0000);
}

esp_err_t gw_zigbee_read_attr(const gw_device_uid_t *uid, uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id)
{
    gw_uart_cmd_req_v1_t req = {0};
    req.cmd_id = GW_UART_CMD_READ_ATTR;
    fill_uid(req.device_uid, uid);
    req.endpoint = endpoint;
    req.cluster_id = cluster_id;
    req.attr_id = attr_id;
    return send_cmd_wait_rsp(&req);
}

esp_err_t gw_zigbee_scene_store(uint16_t group_id, uint8_t scene_id)
{
    (void)group_id;
    (void)scene_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_scene_recall(uint16_t group_id, uint8_t scene_id)
{
    (void)group_id;
    (void)scene_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_bind(const gw_device_uid_t *src_uid, uint8_t src_endpoint, uint16_t cluster_id, const gw_device_uid_t *dst_uid, uint8_t dst_endpoint)
{
    (void)src_uid;
    (void)src_endpoint;
    (void)cluster_id;
    (void)dst_uid;
    (void)dst_endpoint;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_unbind(const gw_device_uid_t *src_uid, uint8_t src_endpoint, uint16_t cluster_id, const gw_device_uid_t *dst_uid, uint8_t dst_endpoint)
{
    (void)src_uid;
    (void)src_endpoint;
    (void)cluster_id;
    (void)dst_uid;
    (void)dst_endpoint;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_binding_table_req(const gw_device_uid_t *uid, uint8_t start_index)
{
    (void)uid;
    (void)start_index;
    return ESP_ERR_NOT_SUPPORTED;
}










