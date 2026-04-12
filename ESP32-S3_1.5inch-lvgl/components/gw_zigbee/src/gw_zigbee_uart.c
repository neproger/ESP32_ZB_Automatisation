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

#include "gw_core/gw_proto_bus.h"
#include "gw_proto/gw_proto_uart.h"

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
#define GW_UART_DRIVER_Q_LEN   8
#define GW_UART_RX_TASK_STACK  9216
#define GW_SNAPSHOT_IDLE_TIMEOUT_US  (3000000LL)
#define GW_SNAPSHOT_RETRY_GAP_US     (1000000LL)
#define GW_SNAPSHOT_RETRY_MAX        6

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
static int32_t s_wait_status;
static bool s_snapshot_stream_active;
static int64_t s_snapshot_last_chunk_us;
static int64_t s_snapshot_last_retry_us;
static uint8_t s_snapshot_retry_count;
static uint16_t s_snapshot_expected_records;
static uint16_t s_snapshot_received_records;
static bool s_bootstrap_ready;
static esp_err_t uart_send_frame(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len);
static esp_err_t send_proto_cmd_wait_result(uint8_t proto_type, const void *payload, uint16_t payload_len);
static esp_err_t request_snapshot_sync(void);

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
    switch (t) {
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
        case GW_PROTO_MSG_SNAPSHOT_REQUEST:
            return "PROTO_SNAPSHOT_REQUEST";
        case GW_PROTO_MSG_CMD_RESULT:
            return "PROTO_CMD_RESULT";
        case GW_PROTO_MSG_CMD_WIFI_CONFIG_SET:
            return "PROTO_CMD_WIFI_CONFIG_SET";
        case GW_PROTO_MSG_CMD_FACTORY_RESET:
            return "PROTO_CMD_FACTORY_RESET";
        case GW_PROTO_MSG_CMD_NET_SERVICES_START:
            return "PROTO_CMD_NET_SERVICES_START";
        case GW_PROTO_MSG_CMD_READ_ATTR:
            return "PROTO_CMD_READ_ATTR";
        case GW_PROTO_MSG_CMD_ONOFF:
            return "PROTO_CMD_ONOFF";
        case GW_PROTO_MSG_CMD_LEVEL:
            return "PROTO_CMD_LEVEL";
        case GW_PROTO_MSG_CMD_COLOR_XY:
            return "PROTO_CMD_COLOR_XY";
        case GW_PROTO_MSG_CMD_COLOR_TEMP:
            return "PROTO_CMD_COLOR_TEMP";
        case GW_PROTO_MSG_CMD_STATE_SYNC:
            return "PROTO_CMD_STATE_SYNC";
        case GW_PROTO_MSG_EVENT_ZB:
            return "PROTO_EVENT_ZB";
        case GW_PROTO_MSG_LINK_ACK:
            return "PROTO_LINK_ACK";
        default:
            return "UNKNOWN";
    }
}

static esp_err_t request_snapshot_sync(void)
{
    esp_err_t err = send_proto_cmd_wait_result(GW_PROTO_MSG_SNAPSHOT_REQUEST, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "snapshot sync request failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "snapshot sync requested");
    }
    return err;
}

static esp_err_t request_proto_async(uint8_t proto_type, const void *payload, uint16_t payload_len, const char *label)
{
    if (!label) {
        label = "proto";
    }
    if (!s_cmd_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_cmd_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t seq = ++s_seq;
    esp_err_t err = uart_send_frame(proto_type, seq, payload, payload_len);
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
    gw_proto_hdr_t hdr = {0};
    uint8_t raw[GW_PROTO_UART_MAX_FRAME_SIZE];
    size_t raw_len = 0;

    if (payload_len > GW_PROTO_UART_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    hdr.version = GW_PROTO_VERSION_V1;
    hdr.type = msg_type;
    hdr.len = payload_len;
    hdr.seq = seq;
    hdr.reserved = 0;

    ESP_RETURN_ON_ERROR(gw_proto_uart_build_frame(&hdr, payload, payload_len, raw, sizeof(raw), &raw_len), TAG, "build_frame failed");
    if (msg_type == GW_PROTO_MSG_SYNC_BEGIN || msg_type == GW_PROTO_MSG_SYNC_END || msg_type == GW_PROTO_MSG_SNAPSHOT_REQUEST) {
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

static void handle_rx_frame(const gw_proto_uart_frame_t *frame)
{
    if (frame->hdr.type == GW_PROTO_MSG_SYNC_BEGIN ||
        frame->hdr.type == GW_PROTO_MSG_SYNC_END ||
        frame->hdr.type == GW_PROTO_MSG_SNAPSHOT_REQUEST) {
        GW_UART_TRACE_I("UART RX %s seq=%u payload=%u", msg_type_name(frame->hdr.type), (unsigned)frame->hdr.seq, (unsigned)frame->hdr.len);
    }

    if (frame->hdr.type == GW_PROTO_MSG_CMD_RESULT) {
        gw_proto_cmd_result_v1_t rsp = {0};
        size_t n = frame->hdr.len < sizeof(rsp) ? frame->hdr.len : sizeof(rsp);
        memcpy(&rsp, frame->payload, n);

        bool match = false;
        portENTER_CRITICAL(&s_wait_lock);
        if (s_wait_active && rsp.request_seq == s_wait_seq) {
            s_wait_status = rsp.status;
            s_wait_active = false;
            match = true;
        }
        portEXIT_CRITICAL(&s_wait_lock);
        if (match) {
            xSemaphoreGive(s_rsp_sem);
        }
        return;
    }

    switch (frame->hdr.type) {
        case GW_PROTO_MSG_SYNC_BEGIN:
        case GW_PROTO_MSG_DEVICE_UPSERT:
        case GW_PROTO_MSG_ENDPOINT_UPSERT:
        case GW_PROTO_MSG_DEVICE_REMOVE:
        case GW_PROTO_MSG_SYNC_END:
            // ACK every topology snapshot frame so the C6 sender can retry missing ones in place.
            (void)uart_send_frame(GW_PROTO_MSG_LINK_ACK, frame->hdr.seq, NULL, 0);
            break;
        default:
            break;
    }

    if (frame->hdr.type == GW_PROTO_MSG_EVENT_ZB && frame->hdr.len >= sizeof(gw_proto_event_v1_t)) {
        const gw_proto_event_v1_t *evt = (const gw_proto_event_v1_t *)frame->payload;
        if (evt->event_id_kind == GW_PROTO_EVENT_NET_STATE) {
            ESP_LOGI(TAG,
                     "Peer net-state event: cmd=%s value=%s ts=%llu",
                     evt->cmd,
                     evt->value_text,
                     (unsigned long long)evt->ts_ms);
            s_bootstrap_ready = false;
            if (!s_snapshot_stream_active) {
                (void)request_proto_async(GW_PROTO_MSG_SNAPSHOT_REQUEST, NULL, 0, "peer-online snapshot sync");
            }
        } else if (evt->event_id_kind == GW_PROTO_EVENT_COMMAND) {
            ESP_LOGI(TAG,
                     "UART RX event command uid=%s short=0x%04x ep=%u cluster=0x%04x cmd=%s",
                     evt->device_uid.uid,
                     evt->short_addr,
                     evt->endpoint,
                     evt->cluster_id,
                     evt->cmd);
        }
    }

    switch (frame->hdr.type) {
        case GW_PROTO_MSG_SYNC_BEGIN:
            s_snapshot_last_chunk_us = esp_timer_get_time();
            s_snapshot_stream_active = true;
            s_snapshot_retry_count = 0;
            s_snapshot_last_retry_us = 0;
            if (frame->hdr.len >= sizeof(gw_proto_sync_begin_v1_t)) {
                const gw_proto_sync_begin_v1_t *msg = (const gw_proto_sync_begin_v1_t *)frame->payload;
                s_snapshot_expected_records = (uint16_t)msg->total_records;
            } else {
                s_snapshot_expected_records = 0;
            }
            s_snapshot_received_records = 0;
            s_bootstrap_ready = false;
            ESP_LOGI(TAG, "Proto sync begin: expected=%u", (unsigned)s_snapshot_expected_records);
            break;
        case GW_PROTO_MSG_SYNC_END:
            s_snapshot_last_chunk_us = esp_timer_get_time();
            s_snapshot_stream_active = false;
            s_snapshot_last_chunk_us = 0;
            s_snapshot_last_retry_us = 0;
            s_snapshot_retry_count = 0;
            break;
        case GW_PROTO_MSG_DEVICE_UPSERT:
        case GW_PROTO_MSG_ENDPOINT_UPSERT:
        case GW_PROTO_MSG_DEVICE_REMOVE:
            s_snapshot_received_records++;
            s_snapshot_last_chunk_us = esp_timer_get_time();
            break;
        default:
            break;
    }

    (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_INGRESS, &frame->hdr, frame->payload);

    if (frame->hdr.type == GW_PROTO_MSG_SYNC_END) {
        ESP_LOGI(TAG, "Proto sync end: expected=%u received=%u",
                 (unsigned)s_snapshot_expected_records,
                 (unsigned)s_snapshot_received_records);
        if (s_snapshot_expected_records > 0 && s_snapshot_received_records < s_snapshot_expected_records) {
            ESP_LOGW(TAG, "Proto sync incomplete, requesting re-sync");
            (void)request_snapshot_sync();
        } else {
            s_bootstrap_ready = true;
            ESP_LOGI(TAG, "Topology snapshot applied; gw_model bootstrap ready");
            (void)request_proto_async(GW_PROTO_MSG_CMD_STATE_SYNC, NULL, 0, "state sync");
        }
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    gw_proto_uart_parser_t parser;
    gw_proto_uart_parser_init(&parser);

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
                         (unsigned)s_snapshot_received_records,
                         (unsigned)s_snapshot_expected_records,
                         (unsigned)s_snapshot_retry_count,
                         (unsigned)GW_SNAPSHOT_RETRY_MAX);
                (void)request_proto_async(GW_PROTO_MSG_SNAPSHOT_REQUEST, NULL, 0, "snapshot sync");
            }
        }
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
                ESP_LOGW(TAG,
                         "UART parse error: %s chunk=%d consumed=%u offset=%u ready=%u",
                         esp_err_to_name(err),
                         n,
                         (unsigned)consumed,
                         (unsigned)off,
                         ready ? 1u : 0u);
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
    esp_err_t err = uart_driver_install(GW_UART_PORT, GW_UART_RX_BUF_SIZE, GW_UART_TX_BUF_SIZE, GW_UART_DRIVER_Q_LEN, NULL, 0);
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

    return ESP_OK;
}

static esp_err_t send_proto_cmd_wait_result(uint8_t proto_type, const void *payload, uint16_t payload_len)
{
    ESP_RETURN_ON_ERROR(ensure_started(), TAG, "uart start failed");

    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(GW_UART_RESP_TIMEOUTMS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t seq = ++s_seq;

    while (xSemaphoreTake(s_rsp_sem, 0) == pdTRUE) {
    }

    portENTER_CRITICAL(&s_wait_lock);
    s_wait_seq = seq;
    s_wait_active = true;
    s_wait_status = ESP_FAIL;
    portEXIT_CRITICAL(&s_wait_lock);

    esp_err_t err = uart_send_frame(proto_type, seq, payload, payload_len);
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

    int32_t status = ESP_FAIL;
    portENTER_CRITICAL(&s_wait_lock);
    status = s_wait_status;
    portEXIT_CRITICAL(&s_wait_lock);

    xSemaphoreGive(s_cmd_lock);
    return (esp_err_t)status;
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
        (void)request_proto_async(GW_PROTO_MSG_SNAPSHOT_REQUEST, NULL, 0, "snapshot sync");
        return ESP_OK;
    }
    return ESP_OK;
}

bool gw_zigbee_bootstrap_ready(void)
{
    return s_bootstrap_ready;
}

esp_err_t gw_zigbee_set_device_name(const gw_device_uid_t *uid, const char *name)
{
    if (!uid || !uid->uid[0] || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_zigbee_remove_device(const gw_device_uid_t *uid)
{
    if (!uid || !uid->uid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_device_remove_v1_t req = {0};
    req.device_uid = *uid;
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_DEVICE_REMOVE, &req, sizeof(req));
}

esp_err_t gw_zigbee_remove_all_devices(void)
{
    gw_proto_cmd_device_remove_all_v1_t req = {0};
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_DEVICE_REMOVE_ALL, &req, sizeof(req));
}

esp_err_t gw_zigbee_factory_reset_peer(void)
{
    gw_proto_cmd_factory_reset_v1_t req = {0};
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_FACTORY_RESET, &req, sizeof(req));
}

esp_err_t gw_zigbee_set_c6_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !password) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ssid_len = strnlen(ssid, 32);
    const size_t pass_len = strnlen(password, 64);
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_wifi_config_set_v1_t req = {0};
    strlcpy(req.ssid, ssid, sizeof(req.ssid));
    strlcpy(req.password, password, sizeof(req.password));
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_WIFI_CONFIG_SET, &req, sizeof(req));
}

esp_err_t gw_zigbee_start_c6_net_services(void)
{
    gw_proto_cmd_net_services_start_v1_t req = {0};
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_NET_SERVICES_START, &req, sizeof(req));
}

esp_err_t gw_zigbee_permit_join(uint8_t seconds)
{
    gw_proto_cmd_permit_join_v1_t req = {0};
    req.seconds = seconds;
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_PERMIT_JOIN, &req, sizeof(req));
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
    if (!uid || !uid->uid[0] || endpoint == 0 || cmd > GW_ZIGBEE_ONOFF_CMD_TOGGLE) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_onoff_v1_t req = {0};
    req.device_uid = *uid;
    req.endpoint = endpoint;
    req.cmd = (uint8_t)cmd;
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_ONOFF, &req, sizeof(req));
}

esp_err_t gw_zigbee_level_move_to_level(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_level_t level)
{
    if (!uid || !uid->uid[0] || endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_level_v1_t req = {0};
    req.device_uid = *uid;
    req.endpoint = endpoint;
    req.level = level.level;
    req.transition_ds = (uint16_t)(level.transition_ms / 100);
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_LEVEL, &req, sizeof(req));
}

esp_err_t gw_zigbee_color_move_to_xy(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_xy_t color)
{
    if (!uid || !uid->uid[0] || endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_color_xy_v1_t req = {0};
    req.device_uid = *uid;
    req.endpoint = endpoint;
    req.x = color.x;
    req.y = color.y;
    req.transition_ds = (uint16_t)(color.transition_ms / 100);
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_COLOR_XY, &req, sizeof(req));
}

esp_err_t gw_zigbee_color_move_to_temp(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_temp_t temp)
{
    if (!uid || !uid->uid[0] || endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_color_temp_v1_t req = {0};
    req.device_uid = *uid;
    req.endpoint = endpoint;
    req.mireds = temp.mireds;
    req.transition_ds = (uint16_t)(temp.transition_ms / 100);
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_COLOR_TEMP, &req, sizeof(req));
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
    if (!uid || !uid->uid[0] || endpoint == 0 || cluster_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_read_attr_v1_t req = {0};
    req.device_uid = *uid;
    req.endpoint = endpoint;
    req.cluster_id = cluster_id;
    req.attr_id = attr_id;
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_READ_ATTR, &req, sizeof(req));
}

esp_err_t gw_zigbee_request_state_sync(void)
{
    return send_proto_cmd_wait_result(GW_PROTO_MSG_CMD_STATE_SYNC, NULL, 0);
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










