#include "gw_uart_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "esp_zigbee_gateway.h"
#include "gw_core/c6_store.h"
#include "gw_proto/gw_proto.h"
#include "gw_proto/gw_proto_types.h"
#include "gw_proto/gw_proto_uart.h"
#include "gw_zigbee/gw_zigbee.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"

#define GW_UART_PORT UART_NUM_1
#define GW_UART_BAUD 230400
#define GW_UART_RX_BUF_SIZE 1024
#define GW_UART_TX_BUF_SIZE 1024
#define GW_UART_DRIVER_Q_LEN 16
static const char *TAG = "gw_uart";

static TaskHandle_t s_rx_task;
static TaskHandle_t s_snapshot_task;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_remove_all_task;
static TaskHandle_t s_state_sync_task;
static TaskHandle_t s_factory_reset_task;
static SemaphoreHandle_t s_tx_lock;
static uint16_t s_evt_seq = 1;
static portMUX_TYPE s_seq_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_snapshot_requested;
static volatile bool s_snapshot_tx_active;
static volatile bool s_snapshot_ready;
static volatile bool s_state_sync_requested;
static esp_timer_handle_t s_snapshot_debounce_timer;
static QueueHandle_t s_tx_queue;
static SemaphoreHandle_t s_ack_sem;
static portMUX_TYPE s_ack_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ack_wait_active;
static uint16_t s_ack_wait_seq;

static const uint32_t GW_SNAPSHOT_DEBOUNCE_MS = 1500;
static const uint32_t GW_UART_TX_PACING_US = 3000;
static const uint32_t GW_REMOVE_ALL_STEP_MS = 6000;
static const size_t GW_UART_TX_QUEUE_LEN = 32;
static const uint32_t GW_SNAPSHOT_ACK_TIMEOUT_MS = 350;
static const uint8_t GW_SNAPSHOT_ACK_RETRIES = 6;
static const uint32_t GW_FACTORY_RESET_DELAY_MS = 500;

#define GW_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT 0x0400
#define GW_ZB_CLUSTER_PRESSURE_MEASUREMENT    0x0403
#define GW_ZB_CLUSTER_OCCUPANCY_SENSING       0x0406
#define GW_ZB_ATTR_MEASURED_VALUE             0x0000
#define GW_ZB_ATTR_OCCUPANCY                  0x0000
#define GW_ZB_ATTR_BATTERY_VOLTAGE            0x0020

typedef struct {
    size_t len;
    uint8_t data[];
} gw_uart_tx_item_t;

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

static void factory_reset_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(GW_FACTORY_RESET_DELAY_MS));

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "gw_data");
    if (!part) {
        ESP_LOGE(TAG, "factory reset failed: gw_data partition not found");
        s_factory_reset_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "factory reset: erasing gw_data size=%u", (unsigned)part->size);
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
        s_factory_reset_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "factory reset complete, restarting");
    esp_restart();
}

static esp_err_t schedule_factory_reset(void)
{
    if (s_factory_reset_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(factory_reset_task, "gw_factory_reset", 3072, NULL, 5, &s_factory_reset_task) != pdPASS) {
        s_factory_reset_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static void uart_send_frame(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len);
static esp_err_t uart_send_frame_sync(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len);
static void snapshot_request_async(void);
static void snapshot_request_debounced(void);
static void snapshot_debounce_timer_cb(void *arg);
static void state_sync_request_async(void);
static esp_err_t exec_proto_command(const gw_proto_uart_frame_t *frame);
static void uart_tx_task(void *arg);
static void remove_all_task(void *arg);
static void uart_state_sync_task(void *arg);

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
        case GW_PROTO_MSG_CMD_FACTORY_RESET:
            return "PROTO_CMD_FACTORY_RESET";
        case GW_PROTO_MSG_EVENT_ZB:
            return "PROTO_EVENT_ZB";
        case GW_PROTO_MSG_LINK_ACK:
            return "PROTO_LINK_ACK";
        default:
            return "UNKNOWN";
    }
}

static esp_err_t uart_send_snapshot(uint16_t base_seq)
{
    const size_t raw_dev_count = gw_c6_store_device_count();
    size_t dev_count = 0;
    size_t endpoint_count = 0;

    for (size_t di = 0; di < raw_dev_count; ++di) {
        gw_device_full_t device = {0};
        if (gw_c6_store_device_get_full_by_index(di, &device) != ESP_OK) {
            continue;
        }
        if (device.status != GW_DEVICE_STATUS_READY) {
            continue;
        }

        size_t device_endpoint_count = 0;
        for (uint8_t ep_idx = 0; ep_idx < device.endpoint_count && ep_idx < GW_DEVICE_MAX_ENDPOINTS; ++ep_idx) {
            const gw_device_endpoint_t *ep = &device.endpoints[ep_idx];
            if (ep->profile_id == 0 && ep->device_id == 0 && ep->in_cluster_count == 0 && ep->out_cluster_count == 0) {
                continue;
            }
            device_endpoint_count++;
        }
        if (device_endpoint_count == 0) {
            continue;
        }

        dev_count++;
        endpoint_count += device_endpoint_count;
    }

    ESP_LOGI(TAG,
             "proto snapshot tx: devices=%u endpoints=%u total=%u",
             (unsigned)dev_count,
             (unsigned)endpoint_count,
             (unsigned)(dev_count + endpoint_count));

    uint16_t seq = base_seq;
    esp_err_t err = ESP_OK;

    // Canonical C6 snapshot is topology-only: device roots first, then endpoint metadata.
    err = uart_send_frame_sync(GW_PROTO_MSG_SYNC_BEGIN,
                               seq++,
                               &(gw_proto_sync_begin_v1_t){
                                   .scope = GW_PROTO_SYNC_SCOPE_FULL,
                                   .reserved0 = 0,
                                   .reserved1 = 0,
                                   .total_records = (uint32_t)(dev_count + endpoint_count),
                               },
                               sizeof(gw_proto_sync_begin_v1_t));
    if (err != ESP_OK) {
        goto fail_sync_begin;
    }

    // Phase 1: send only device topology roots first.
    for (size_t di = 0; di < raw_dev_count; ++di) {
        gw_device_full_t device = {0};
        if (gw_c6_store_device_get_full_by_index(di, &device) != ESP_OK) {
            continue;
        }
        if (device.status != GW_DEVICE_STATUS_READY) {
            continue;
        }

        bool has_endpoint = false;
        for (uint8_t ep_idx = 0; ep_idx < device.endpoint_count && ep_idx < GW_DEVICE_MAX_ENDPOINTS; ++ep_idx) {
            const gw_device_endpoint_t *ep = &device.endpoints[ep_idx];
            if (ep->profile_id == 0 && ep->device_id == 0 && ep->in_cluster_count == 0 && ep->out_cluster_count == 0) {
                continue;
            }
            has_endpoint = true;
            break;
        }
        if (!has_endpoint) {
            continue;
        }
        gw_proto_device_v1_t msg = {0};
        msg.device_uid = device.device_uid;
        msg.short_addr = device.short_addr;
        strlcpy(msg.name, device.name, sizeof(msg.name));
        msg.version = 0;
        msg.last_seen_ms = device.last_seen_ms;
        msg.has_onoff = device.has_onoff ? 1u : 0u;
        msg.has_button = device.has_button ? 1u : 0u;
        msg.status = (uint8_t)device.status;
        err = uart_send_frame_sync(GW_PROTO_MSG_DEVICE_UPSERT, seq++, &msg, sizeof(msg));
        if (err != ESP_OK) {
            goto fail_device;
        }
    }

    // Phase 2: stream endpoint metadata for the same device.
    for (size_t di = 0; di < raw_dev_count; ++di) {
        gw_device_full_t device = {0};
        if (gw_c6_store_device_get_full_by_index(di, &device) != ESP_OK) {
            continue;
        }
        if (device.status != GW_DEVICE_STATUS_READY) {
            continue;
        }

        for (uint8_t ep_idx = 0; ep_idx < device.endpoint_count && ep_idx < GW_DEVICE_MAX_ENDPOINTS; ++ep_idx) {
            const uint8_t endpoint = (uint8_t)(ep_idx + 1u);
            const gw_device_endpoint_t *ep = &device.endpoints[ep_idx];
            if (ep->profile_id == 0 && ep->device_id == 0 && ep->in_cluster_count == 0 && ep->out_cluster_count == 0) {
                continue;
            }
            gw_proto_endpoint_v1_t msg = {0};
            msg.uid = device.device_uid;
            msg.short_addr = device.short_addr;
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
            strlcpy(msg.kind, ep->kind, sizeof(msg.kind));
            err = uart_send_frame_sync(GW_PROTO_MSG_ENDPOINT_UPSERT, seq++, &msg, sizeof(msg));
            if (err != ESP_OK) {
                goto fail_endpoint;
            }
        }
    }

    err = uart_send_frame_sync(GW_PROTO_MSG_SYNC_END,
                               seq++,
                               &(gw_proto_sync_end_v1_t){
                                   .scope = GW_PROTO_SYNC_SCOPE_FULL,
                                   .status = 0,
                                   .reserved0 = 0,
                                   .total_records = (uint32_t)(dev_count + endpoint_count),
                               },
                               sizeof(gw_proto_sync_end_v1_t));
    if (err != ESP_OK) {
        ESP_RETURN_ON_ERROR(err, TAG, "snapshot sync_end failed");
    }

    return ESP_OK;

fail_endpoint:
    ESP_LOGE(TAG, "snapshot endpoint failed: %s", esp_err_to_name(err));
    return err;

fail_device:
    ESP_LOGE(TAG, "snapshot device failed: %s", esp_err_to_name(err));
    return err;

fail_sync_begin:
    ESP_LOGE(TAG, "snapshot sync_begin failed: %s", esp_err_to_name(err));
    return err;
}

static void snapshot_request_async(void)
{
    s_snapshot_requested = true;
    if (s_snapshot_ready && s_snapshot_task) {
        xTaskNotifyGive(s_snapshot_task);
    }
}

static void snapshot_debounce_timer_cb(void *arg)
{
    (void)arg;
    snapshot_request_async();
}

static bool endpoint_has_cluster(const gw_device_endpoint_t *ep, uint16_t cluster_id)
{
    if (!ep || cluster_id == 0) {
        return false;
    }
    for (uint8_t i = 0; i < ep->in_cluster_count && i < GW_DEVICE_MAX_CLUSTERS; ++i) {
        if (ep->in_clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static esp_err_t request_endpoint_state(const gw_device_uid_t *uid, uint8_t endpoint, const gw_device_endpoint_t *ep)
{
    if (!uid || !uid->uid[0] || endpoint == 0 || !ep) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t first_err = ESP_OK;
    const struct {
        uint16_t cluster_id;
        uint16_t attr_id;
    } reqs[] = {
        { ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID },
        { ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, GW_ZB_ATTR_BATTERY_VOLTAGE },
        { GW_ZB_CLUSTER_OCCUPANCY_SENSING, GW_ZB_ATTR_OCCUPANCY },
        { GW_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, GW_ZB_ATTR_MEASURED_VALUE },
        { GW_ZB_CLUSTER_PRESSURE_MEASUREMENT, GW_ZB_ATTR_MEASURED_VALUE },
    };

    for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); ++i) {
        if (!endpoint_has_cluster(ep, reqs[i].cluster_id)) {
            continue;
        }
        esp_err_t err = gw_zigbee_read_attr(uid, endpoint, reqs[i].cluster_id, reqs[i].attr_id);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    return first_err;
}

static void state_sync_request_async(void)
{
    s_state_sync_requested = true;
    if (s_state_sync_task) {
        xTaskNotifyGive(s_state_sync_task);
    }
}

static void snapshot_request_debounced(void)
{
    if (!s_snapshot_debounce_timer) {
        snapshot_request_async();
        return;
    }
    (void)esp_timer_stop(s_snapshot_debounce_timer);
    if (esp_timer_start_once(s_snapshot_debounce_timer, (uint64_t)GW_SNAPSHOT_DEBOUNCE_MS * 1000ULL) != ESP_OK) {
        snapshot_request_async();
    }
}

static uint16_t next_evt_seq(void)
{
    uint16_t seq = 0;
    portENTER_CRITICAL(&s_seq_lock);
    seq = s_evt_seq++;
    portEXIT_CRITICAL(&s_seq_lock);
    return seq;
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
    gw_uart_tx_item_t *item = NULL;

    if (payload_len > GW_PROTO_UART_MAX_PAYLOAD) {
        return;
    }
    if (gw_proto_uart_build_frame(&hdr, payload, payload_len, raw, sizeof(raw), &raw_len) != ESP_OK) {
        return;
    }
    if (!s_tx_queue) {
        ESP_LOGW(TAG, "UART TX queue not ready, drop %s seq=%u len=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)raw_len);
        return;
    }

    item = (gw_uart_tx_item_t *)malloc(sizeof(*item) + raw_len);
    if (!item) {
        ESP_LOGW(TAG, "UART TX alloc failed %s seq=%u len=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)raw_len);
        return;
    }
    item->len = raw_len;
    memcpy(item->data, raw, raw_len);

    if (xQueueSend(s_tx_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UART TX queue full, drop %s seq=%u len=%u", msg_type_name(msg_type), (unsigned)seq, (unsigned)raw_len);
        free(item);
    }
}

static esp_err_t uart_send_frame_sync(uint8_t msg_type, uint16_t seq, const void *payload, uint16_t payload_len)
{
    uint8_t raw[GW_PROTO_UART_MAX_FRAME_SIZE];
    size_t raw_len = 0;
    gw_proto_hdr_t hdr = {
        .version = GW_PROTO_VERSION_V1,
        .type = msg_type,
        .len = payload_len,
        .seq = seq,
        .reserved = 0,
    };

    if (payload_len > GW_PROTO_UART_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(gw_proto_uart_build_frame(&hdr, payload, payload_len, raw, sizeof(raw), &raw_len), TAG, "build sync frame failed");

    // Snapshot transport is intentionally stop-and-wait: resend the same frame until S3 ACKs it.
    for (uint8_t attempt = 0; attempt < GW_SNAPSHOT_ACK_RETRIES; ++attempt) {
        xQueueReset(s_ack_sem);
        portENTER_CRITICAL(&s_ack_lock);
        s_ack_wait_seq = seq;
        s_ack_wait_active = true;
        portEXIT_CRITICAL(&s_ack_lock);

        if (s_tx_lock) {
            (void)xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        }
        bool ok = uart_write_all(raw, raw_len);
        if (s_tx_lock) {
            (void)xSemaphoreGive(s_tx_lock);
        }
        if (!ok) {
            portENTER_CRITICAL(&s_ack_lock);
            s_ack_wait_active = false;
            portEXIT_CRITICAL(&s_ack_lock);
            continue;
        }

        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(GW_SNAPSHOT_ACK_TIMEOUT_MS)) == pdTRUE) {
            return ESP_OK;
        }

        portENTER_CRITICAL(&s_ack_lock);
        s_ack_wait_active = false;
        portEXIT_CRITICAL(&s_ack_lock);
        ESP_LOGW(TAG,
                 "snapshot ack timeout, resend %s seq=%u attempt=%u/%u",
                 msg_type_name(msg_type),
                 (unsigned)seq,
                 (unsigned)(attempt + 1),
                 (unsigned)GW_SNAPSHOT_ACK_RETRIES);
    }

    return ESP_ERR_TIMEOUT;
}

static void uart_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        gw_uart_tx_item_t *item = NULL;
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!item) {
            continue;
        }

        if (s_tx_lock) {
            (void)xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        }
        bool ok = uart_write_all(item->data, item->len);
        if (s_tx_lock) {
            (void)xSemaphoreGive(s_tx_lock);
        }
        if (!ok) {
            ESP_LOGW(TAG, "UART TX drop frame len=%u", (unsigned)item->len);
        }
        free(item);

        if (GW_UART_TX_PACING_US > 0) {
            esp_rom_delay_us(GW_UART_TX_PACING_US);
        }
    }
}

static void remove_all_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        typedef struct {
            gw_device_uid_t uid;
            uint16_t short_addr;
        } remove_item_t;

        remove_item_t remove_list[GW_DEVICE_MAX_DEVICES] = {0};
        size_t remove_count = 0;
        const size_t dev_count = gw_c6_store_device_count();

        for (size_t i = 0; i < dev_count && remove_count < GW_DEVICE_MAX_DEVICES; ++i) {
            gw_device_full_t device = {0};
            if (gw_c6_store_device_get_full_by_index(i, &device) != ESP_OK) {
                continue;
            }
            if (device.device_uid.uid[0] == '\0' || device.short_addr == 0 || device.short_addr == 0xFFFF) {
                continue;
            }
            remove_list[remove_count].uid = device.device_uid;
            remove_list[remove_count].short_addr = device.short_addr;
            remove_count++;
        }

        // Purge local live/runtime state first so stale NVS-backed devices disappear immediately.
        for (size_t i = 0; i < remove_count; ++i) {
            (void)gw_c6_store_device_remove(&remove_list[i].uid);
        }
        snapshot_request_async();

        for (size_t i = 0; i < remove_count; ++i) {
            if (remove_list[i].uid.uid[0] == '\0' || remove_list[i].short_addr == 0 || remove_list[i].short_addr == 0xFFFF) {
                continue;
            }

            esp_err_t err = gw_zigbee_device_leave(&remove_list[i].uid, remove_list[i].short_addr, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "remove_all leave schedule failed uid=%s short=0x%04x err=%s",
                         remove_list[i].uid.uid,
                         remove_list[i].short_addr,
                         esp_err_to_name(err));
            }

            vTaskDelay(pdMS_TO_TICKS(GW_REMOVE_ALL_STEP_MS));
        }
    }
}

static void uart_state_sync_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_state_sync_requested) {
            continue;
        }

        while (s_state_sync_requested) {
            s_state_sync_requested = false;

            const size_t dev_count = gw_c6_store_device_count();
            size_t endpoint_count = 0;
            ESP_LOGI(TAG, "state sync begin: devices=%u", (unsigned)dev_count);

            for (size_t di = 0; di < dev_count; ++di) {
                gw_device_full_t device = {0};
                if (gw_c6_store_device_get_full_by_index(di, &device) != ESP_OK) {
                    continue;
                }
                if (device.device_uid.uid[0] == '\0' || device.short_addr == 0 || device.short_addr == 0xFFFF) {
                    continue;
                }
                if (device.status != GW_DEVICE_STATUS_READY) {
                    continue;
                }

                for (uint8_t ep_idx = 0; ep_idx < device.endpoint_count && ep_idx < GW_DEVICE_MAX_ENDPOINTS; ++ep_idx) {
                    const uint8_t endpoint = (uint8_t)(ep_idx + 1u);
                    const gw_device_endpoint_t *ep = &device.endpoints[ep_idx];
                    if (ep->profile_id == 0 && ep->device_id == 0 &&
                        ep->in_cluster_count == 0 && ep->out_cluster_count == 0) {
                        continue;
                    }
                    (void)request_endpoint_state(&device.device_uid, endpoint, ep);
                    endpoint_count++;
                }
            }

            ESP_LOGI(TAG, "state sync queued: endpoints=%u", (unsigned)endpoint_count);
        }
    }
}

static void uart_send_proto_cmd_result(uint16_t seq, esp_err_t err)
{
    gw_proto_cmd_result_v1_t rsp = {
        .request_seq = seq,
        .status = (int32_t)err,
    };
    uart_send_frame(GW_PROTO_MSG_CMD_RESULT, seq, &rsp, sizeof(rsp));
}

static void handle_rx_frame(const gw_proto_uart_frame_t *frame)
{
    if (frame->hdr.type == GW_PROTO_MSG_LINK_ACK) {
        bool match = false;
        portENTER_CRITICAL(&s_ack_lock);
        if (s_ack_wait_active && frame->hdr.seq == s_ack_wait_seq) {
            s_ack_wait_active = false;
            match = true;
        }
        portEXIT_CRITICAL(&s_ack_lock);
        if (match && s_ack_sem) {
            xSemaphoreGive(s_ack_sem);
        }
        return;
    }

    switch (frame->hdr.type) {
        case GW_PROTO_MSG_SNAPSHOT_REQUEST:
        case GW_PROTO_MSG_CMD_PERMIT_JOIN:
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE:
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE_ALL:
        case GW_PROTO_MSG_CMD_WIFI_CONFIG_SET:
        case GW_PROTO_MSG_CMD_NET_SERVICES_START:
        case GW_PROTO_MSG_CMD_READ_ATTR:
        case GW_PROTO_MSG_CMD_ONOFF:
        case GW_PROTO_MSG_CMD_LEVEL:
        case GW_PROTO_MSG_CMD_COLOR_XY:
        case GW_PROTO_MSG_CMD_COLOR_TEMP:
        case GW_PROTO_MSG_CMD_STATE_SYNC:
        case GW_PROTO_MSG_CMD_FACTORY_RESET: {
            esp_err_t err = exec_proto_command(frame);
            uart_send_proto_cmd_result(frame->hdr.seq, err);
            break;
        }
        default:
            break;
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
            if (!s_snapshot_ready) {
                break;
            }
            s_snapshot_requested = false;
            s_snapshot_tx_active = true;
            (void)uart_send_snapshot(next_evt_seq());
            s_snapshot_tx_active = false;
        }
    }
}

static esp_err_t exec_proto_command(const gw_proto_uart_frame_t *frame)
{
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (frame->hdr.type) {
        case GW_PROTO_MSG_SNAPSHOT_REQUEST:
            snapshot_request_async();
            return ESP_OK;

        case GW_PROTO_MSG_CMD_PERMIT_JOIN: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_permit_join_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_permit_join_v1_t *msg = (const gw_proto_cmd_permit_join_v1_t *)frame->payload;
            return gw_zigbee_permit_join(msg->seconds);
        }

        case GW_PROTO_MSG_CMD_DEVICE_REMOVE: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_device_remove_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_device_remove_v1_t *msg = (const gw_proto_cmd_device_remove_v1_t *)frame->payload;
            ESP_LOGI(TAG, "cmd device_remove uid=%s", msg->device_uid.uid);
            gw_device_full_t device = {0};
            esp_err_t err = gw_c6_store_device_get_full(&msg->device_uid, &device);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "cmd device_remove uid=%s registry=%s",
                         msg->device_uid.uid,
                         esp_err_to_name(err));
                return err;
            }
            if (device.short_addr == 0) {
                ESP_LOGW(TAG, "cmd device_remove uid=%s invalid short=0x%04x", msg->device_uid.uid, (unsigned)device.short_addr);
                return ESP_ERR_INVALID_STATE;
            }
            ESP_LOGI(TAG,
                     "cmd device_remove uid=%s short=0x%04x status=%u",
                     msg->device_uid.uid,
                     (unsigned)device.short_addr,
                     (unsigned)device.status);
            return gw_zigbee_device_leave(&msg->device_uid, device.short_addr, false);
        }

        case GW_PROTO_MSG_CMD_DEVICE_REMOVE_ALL: {
            if (s_remove_all_task == NULL) {
                return ESP_ERR_INVALID_STATE;
            }
            ESP_LOGW(TAG,
                     "cmd device_remove_all requested devices=%u",
                     (unsigned)gw_c6_store_device_count());
            xTaskNotifyGive(s_remove_all_task);
            return ESP_OK;
        }

        case GW_PROTO_MSG_CMD_FACTORY_RESET: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_factory_reset_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            ESP_LOGW(TAG, "cmd factory_reset requested");
            return schedule_factory_reset();
        }

        case GW_PROTO_MSG_CMD_WIFI_CONFIG_SET:
            return ESP_ERR_NOT_SUPPORTED;

        case GW_PROTO_MSG_CMD_NET_SERVICES_START:
            return ESP_ERR_NOT_SUPPORTED;

        case GW_PROTO_MSG_CMD_READ_ATTR: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_read_attr_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_read_attr_v1_t *msg = (const gw_proto_cmd_read_attr_v1_t *)frame->payload;
            if (msg->endpoint == 0 || msg->cluster_id == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_zigbee_read_attr(&msg->device_uid, msg->endpoint, msg->cluster_id, msg->attr_id);
        }

        case GW_PROTO_MSG_CMD_ONOFF: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_onoff_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_onoff_v1_t *msg = (const gw_proto_cmd_onoff_v1_t *)frame->payload;
            if (msg->endpoint == 0 || msg->cmd > GW_ZIGBEE_ONOFF_CMD_TOGGLE) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_zigbee_onoff_cmd(&msg->device_uid, msg->endpoint, (gw_zigbee_onoff_cmd_t)msg->cmd);
        }

        case GW_PROTO_MSG_CMD_LEVEL: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_level_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_level_v1_t *msg = (const gw_proto_cmd_level_v1_t *)frame->payload;
            if (msg->endpoint == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_level_t lv = {
                .level = msg->level,
                .transition_ms = (uint16_t)(msg->transition_ds * 100u),
            };
            return gw_zigbee_level_move_to_level(&msg->device_uid, msg->endpoint, lv);
        }

        case GW_PROTO_MSG_CMD_COLOR_XY: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_color_xy_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_color_xy_v1_t *msg = (const gw_proto_cmd_color_xy_v1_t *)frame->payload;
            if (msg->endpoint == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_xy_t xy = {
                .x = msg->x,
                .y = msg->y,
                .transition_ms = (uint16_t)(msg->transition_ds * 100u),
            };
            return gw_zigbee_color_move_to_xy(&msg->device_uid, msg->endpoint, xy);
        }

        case GW_PROTO_MSG_CMD_COLOR_TEMP: {
            if (frame->hdr.len < sizeof(gw_proto_cmd_color_temp_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_color_temp_v1_t *msg = (const gw_proto_cmd_color_temp_v1_t *)frame->payload;
            if (msg->endpoint == 0 || msg->mireds == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_temp_t ct = {
                .mireds = msg->mireds,
                .transition_ms = (uint16_t)(msg->transition_ds * 100u),
            };
            return gw_zigbee_color_move_to_temp(&msg->device_uid, msg->endpoint, ct);
        }

        case GW_PROTO_MSG_CMD_STATE_SYNC:
            state_sync_request_async();
            return ESP_OK;

        default:
            return ESP_ERR_NOT_SUPPORTED;
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

    ESP_RETURN_ON_ERROR(uart_driver_install(GW_UART_PORT, GW_UART_RX_BUF_SIZE, GW_UART_TX_BUF_SIZE, GW_UART_DRIVER_Q_LEN, NULL, 0), TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(GW_UART_PORT, &cfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(GW_UART_PORT, GW_UART_TX_PIN, GW_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin failed");

    s_tx_queue = xQueueCreate(GW_UART_TX_QUEUE_LEN, sizeof(gw_uart_tx_item_t *));
    if (!s_tx_queue) {
        return ESP_ERR_NO_MEM;
    }
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_ack_sem = xSemaphoreCreateBinary();
    if (!s_ack_sem) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t debounce_timer_args = {
        .callback = snapshot_debounce_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "c6_snap_db",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&debounce_timer_args, &s_snapshot_debounce_timer), TAG, "esp_timer_create failed");

    if (xTaskCreate(uart_snapshot_task, "uart_snap", 9216, NULL, 6, &s_snapshot_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uart_tx_task, "uart_tx", 4096, NULL, 6, &s_tx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(remove_all_task, "uart_rm_all", 4096, NULL, 5, &s_remove_all_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uart_state_sync_task, "uart_state", 4096, NULL, 5, &s_state_sync_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 6, &s_rx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_snapshot_ready = false;
    ESP_LOGI(TAG, "UART binary link started: UART1 TX=GPIO%d RX=GPIO%d baud=%d", GW_UART_TX_PIN, GW_UART_RX_PIN, GW_UART_BAUD);
    return ESP_OK;
}

void gw_uart_link_set_snapshot_ready(bool ready)
{
    s_snapshot_ready = ready;
    if (ready && s_snapshot_requested && s_snapshot_task) {
        xTaskNotifyGive(s_snapshot_task);
    }
}

esp_err_t gw_uart_link_send_event_zb(const gw_proto_event_v1_t *event)
{
    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tx_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    // Keep live events off the wire while the topology snapshot is in flight.
    if (s_snapshot_tx_active) {
        return ESP_ERR_INVALID_STATE;
    }
    uart_send_frame(GW_PROTO_MSG_EVENT_ZB, next_evt_seq(), event, sizeof(*event));
    return ESP_OK;
}

esp_err_t gw_uart_link_request_snapshot_debounced(void)
{
    snapshot_request_debounced();
    return ESP_OK;
}
