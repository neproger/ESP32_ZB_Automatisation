#include "gw_http/gw_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_core/action_exec.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_model/gw_model_automation.h"
#include "gw_model/gw_model_groups.h"
#include "gw_model/gw_model_settings.h"
#include "gw_model/gw_model_state.h"
#include "gw_model/gw_model_topology.h"
#include "gw_proto/gw_proto_frame.h"
#include "gw_proto/gw_proto_map.h"
#include "gw_proto/gw_proto_snapshot.h"
#include "gw_zigbee/gw_zigbee.h"

static const char *TAG = "gw_ws";
static const bool kWsUsePsram = true;

typedef struct {
    int fd;
    bool subscribed_events;
} gw_ws_client_t;

typedef struct {
    int fd;
    uint8_t *data;
    size_t len;
} ws_tx_msg_t;

static httpd_handle_t s_server;
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_ws_seq = 1;

#define GW_WS_MAX_CLIENTS 6
#define GW_WS_TX_Q_CAP 32
#define GW_WS_TX_TASK_PRIO 2
#define GW_WS_TX_TASK_STACK 3072
static gw_ws_client_t s_clients[GW_WS_MAX_CLIENTS];
static QueueHandle_t s_tx_q;
static TaskHandle_t s_tx_task;
static StaticQueue_t s_tx_q_struct;
static uint8_t s_tx_q_storage[GW_WS_TX_Q_CAP * sizeof(ws_tx_msg_t)];

static size_t ws_collect_client_fds(int *fds, size_t max_fds);
static esp_err_t ws_send_binary_async(int fd, const void *buf, size_t len);
static esp_err_t ws_handle_proto_command(uint8_t type, const uint8_t *payload, uint16_t payload_len);
static esp_err_t ws_route_proto_frame(httpd_req_t *req, int fd, const uint8_t *frame_buf, size_t frame_len);
static esp_err_t ws_send_proto_snapshot_sync(httpd_req_t *req, int fd);
static void ws_on_proto_bus_message(gw_proto_bus_channel_t channel, const gw_proto_hdr_t *hdr, const void *payload, void *user_ctx);
static esp_err_t ws_remove_all_automations(void);
static esp_err_t ws_schedule_factory_reset(void);
static esp_err_t ws_handle_device_change(const gw_proto_cmd_device_change_v1_t *msg);
static esp_err_t ws_handle_group_change(const gw_proto_cmd_group_change_v1_t *msg);
static esp_err_t ws_handle_automation_change(const gw_proto_cmd_automation_change_v1_t *msg);
static esp_err_t ws_handle_settings_change(const gw_proto_cmd_settings_change_v1_t *msg);
static esp_err_t ws_handle_group_items_change(const gw_proto_cmd_group_items_change_v1_t *msg);

static TaskHandle_t s_factory_reset_task;

static bool ws_fd_is_alive(int fd)
{
    if (!s_server || fd <= 0) {
        return false;
    }
    return httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static void ws_factory_reset_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(700));

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

static esp_err_t ws_schedule_factory_reset(void)
{
    if (s_factory_reset_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(ws_factory_reset_task, "s3_factory_reset", 3072, NULL, 5, &s_factory_reset_task) != pdPASS) {
        s_factory_reset_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t ws_handle_device_change(const gw_proto_cmd_device_change_v1_t *msg)
{
    if (!msg || !msg->device_uid.uid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_device_v1_t device = {0};
    esp_err_t err = gw_model_get_device(&msg->device_uid, &device);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CMD_DEVICE_CHANGE: device not found uid=%s err=%s",
                 msg->device_uid.uid,
                 esp_err_to_name(err));
        return err;
    }

    if (msg->fields & GW_PROTO_DEVICE_CHANGE_F_NAME) {
        strlcpy(device.name, msg->name, sizeof(device.name));
    }

    bool changed = false;
    bool inserted = false;
    err = gw_model_upsert_device(&device, &changed, &inserted);
    return err;
}

static esp_err_t ws_handle_group_change(const gw_proto_cmd_group_change_v1_t *msg)
{
    if (!msg || !msg->id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (msg->fields & GW_PROTO_GROUP_CHANGE_F_NAME) {
        return gw_model_rename_group(msg->id, msg->name);
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t ws_handle_automation_change(const gw_proto_cmd_automation_change_v1_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((msg->fields & GW_PROTO_AUTOMATION_CHANGE_F_ENTRY) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!msg->entry.id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_model_upsert_automation(&msg->entry, NULL, NULL);
}

static esp_err_t ws_handle_settings_change(const gw_proto_cmd_settings_change_v1_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((msg->fields & GW_PROTO_SETTINGS_CHANGE_F_ALL) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gw_model_settings_validate(&msg->settings)) {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_model_set_settings(&msg->settings, NULL, NULL);
}

static esp_err_t ws_handle_group_items_change(const gw_proto_cmd_group_items_change_v1_t *msg)
{
    if (!msg || !msg->device_uid.uid[0] || msg->endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (msg->op) {
        case GW_PROTO_GROUP_ITEMS_OP_SET:
            if (!msg->group_id[0]) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_model_set_group_item(msg->group_id, &msg->device_uid, msg->endpoint);
        case GW_PROTO_GROUP_ITEMS_OP_REMOVE:
            return gw_model_remove_group_item_by_endpoint(&msg->device_uid, msg->endpoint);
        case GW_PROTO_GROUP_ITEMS_OP_REORDER:
            if (!msg->group_id[0] || msg->order == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_model_reorder_group_item(msg->group_id, &msg->device_uid, msg->endpoint, msg->order);
        case GW_PROTO_GROUP_ITEMS_OP_LABEL:
            return gw_model_set_group_item_label(&msg->device_uid, msg->endpoint, msg->label);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static void ws_prune_stale_clients_locked(void)
{
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd != 0 && !ws_fd_is_alive(s_clients[i].fd)) {
            s_clients[i] = (gw_ws_client_t){0};
        }
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
}

static bool ws_client_add_fd(int fd)
{
    bool ok = false;
    portENTER_CRITICAL(&s_client_lock);
    ws_prune_stale_clients_locked();
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
    return ok;
}

static esp_err_t ws_send_proto_frame_async(int fd, uint8_t type, uint16_t seq, const void *payload, uint16_t payload_len)
{
    uint8_t frame_buf[GW_PROTO_FRAME_MAX_SIZE];
    size_t frame_len = 0;
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, type, payload_len, seq);
    esp_err_t err = gw_proto_frame_build(&hdr, payload, payload_len, frame_buf, sizeof(frame_buf), &frame_len);
    if (err != ESP_OK) {
        return err;
    }
    return ws_send_binary_async(fd, frame_buf, frame_len);
}

static esp_err_t ws_send_proto_frame_sync(httpd_req_t *req, uint8_t type, uint16_t seq, const void *payload, uint16_t payload_len)
{
    if (!req) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t frame_buf[GW_PROTO_FRAME_MAX_SIZE];
    size_t frame_len = 0;
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, type, payload_len, seq);
    esp_err_t err = gw_proto_frame_build(&hdr, payload, payload_len, frame_buf, sizeof(frame_buf), &frame_len);
    if (err != ESP_OK) {
        return err;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = frame_buf,
        .len = frame_len,
    };
    return httpd_ws_send_frame(req, &frame);
}

static esp_err_t ws_send_binary_async(int fd, const void *buf, size_t len)
{
    if (!s_server || !s_tx_q || fd <= 0 || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *copy = NULL;
    if (kWsUsePsram) {
        copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!copy) {
        copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!copy) {
        copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buf, len);

    ws_tx_msg_t msg = {
        .fd = fd,
        .data = copy,
        .len = len,
    };
    if (xQueueSend(s_tx_q, &msg, 0) != pdTRUE) {
        free(copy);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t ws_handle_proto_command(uint8_t type, const uint8_t *payload, uint16_t payload_len)
{
    if (!payload && payload_len != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (type) {
        case GW_PROTO_MSG_CMD_PERMIT_JOIN: {
            if (payload_len < sizeof(gw_proto_cmd_permit_join_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_permit_join_v1_t *msg = (const gw_proto_cmd_permit_join_v1_t *)payload;
            const uint8_t seconds = msg->seconds > 0 ? msg->seconds : 180;
            return gw_zigbee_permit_join(seconds);
        }
        case GW_PROTO_MSG_CMD_DEVICE_CHANGE: {
            if (payload_len < sizeof(gw_proto_cmd_device_change_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            return ws_handle_device_change((const gw_proto_cmd_device_change_v1_t *)payload);
        }
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE: {
            if (payload_len < sizeof(gw_proto_cmd_device_remove_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_device_remove_v1_t *msg = (const gw_proto_cmd_device_remove_v1_t *)payload;
            bool removed = false;
            esp_err_t err = gw_model_remove_full_device(&msg->device_uid, &removed);
            if (err != ESP_OK) {
                return err;
            }
            if (removed) {
                (void)gw_zigbee_remove_device(&msg->device_uid);
            }
            return ESP_OK;
        }
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE_ALL: {
            if (payload_len < sizeof(gw_proto_cmd_device_remove_all_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const size_t count = gw_model_count_devices();
            for (size_t i = 0; i < count; ++i) {
                gw_proto_device_v1_t device = {0};
                if (gw_model_get_device_by_index(0, &device) != ESP_OK) {
                    break;
                }
                if (device.device_uid.uid[0]) {
                    (void)gw_model_remove_full_device(&device.device_uid, NULL);
                }
            }
            return gw_zigbee_remove_all_devices();
        }
        case GW_PROTO_MSG_CMD_GROUP_CREATE: {
            if (payload_len < sizeof(gw_proto_cmd_group_create_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_create_v1_t *msg = (const gw_proto_cmd_group_create_v1_t *)payload;
            gw_proto_group_v1_t created = {0};
            return gw_model_create_group(msg->id[0] ? msg->id : NULL, msg->name, &created);
        }
        case GW_PROTO_MSG_CMD_GROUP_CHANGE: {
            if (payload_len < sizeof(gw_proto_cmd_group_change_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            return ws_handle_group_change((const gw_proto_cmd_group_change_v1_t *)payload);
        }
        case GW_PROTO_MSG_CMD_GROUP_DELETE: {
            if (payload_len < sizeof(gw_proto_cmd_group_delete_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_delete_v1_t *msg = (const gw_proto_cmd_group_delete_v1_t *)payload;
            return gw_model_remove_group(msg->id, NULL);
        }
        case GW_PROTO_MSG_CMD_GROUP_ITEMS_CHANGE: {
            if (payload_len < sizeof(gw_proto_cmd_group_items_change_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            return ws_handle_group_items_change((const gw_proto_cmd_group_items_change_v1_t *)payload);
        }
        case GW_PROTO_MSG_CMD_SETTINGS_CHANGE: {
            if (payload_len < sizeof(gw_proto_cmd_settings_change_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            return ws_handle_settings_change((const gw_proto_cmd_settings_change_v1_t *)payload);
        }
        case GW_PROTO_MSG_CMD_FACTORY_RESET: {
            if (payload_len < sizeof(gw_proto_cmd_factory_reset_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            esp_err_t peer_err = gw_zigbee_factory_reset_peer();
            if (peer_err != ESP_OK) {
                ESP_LOGW(TAG, "peer factory reset request failed: %s", esp_err_to_name(peer_err));
            }
            return ws_schedule_factory_reset();
        }
        case GW_PROTO_MSG_CMD_AUTOMATION_REMOVE: {
            if (payload_len < sizeof(gw_proto_cmd_automation_remove_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_automation_remove_v1_t *msg = (const gw_proto_cmd_automation_remove_v1_t *)payload;
            return gw_model_remove_automation(msg->id, NULL);
        }
        case GW_PROTO_MSG_CMD_AUTOMATION_RESET_ALL: {
            if (payload_len < sizeof(gw_proto_cmd_automation_reset_all_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            return ws_remove_all_automations();
        }
        case GW_PROTO_MSG_CMD_AUTOMATION_CHANGE: {
            if (payload_len != sizeof(gw_proto_cmd_automation_change_v1_t)) {
                ESP_LOGW(TAG, "automation_change: invalid size %u (expected %u)",
                         payload_len, sizeof(gw_proto_cmd_automation_change_v1_t));
                return ESP_ERR_INVALID_SIZE;
            }
            return ws_handle_automation_change((const gw_proto_cmd_automation_change_v1_t *)payload);
        }
        case GW_PROTO_MSG_CMD_ACTION_EXEC: {
            if (payload_len != sizeof(gw_automation_entry_t)) {
                ESP_LOGW(TAG, "action_exec: invalid size %u (expected %u)",
                         payload_len, sizeof(gw_automation_entry_t));
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_automation_entry_t *msg = (const gw_automation_entry_t *)payload;
            char errbuf[128] = {0};
            esp_err_t rc = gw_action_exec_entry(msg, errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "action_exec failed: %s", errbuf);
            }
            return rc;
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

typedef struct {
    size_t group_count;
    size_t group_item_count;
    size_t group_index;
    size_t item_index;
    gw_proto_group_v1_t group;
    gw_proto_group_item_v1_t item;
} ws_group_snapshot_iter_t;

typedef struct {
    size_t count;
    size_t index;
    gw_automation_entry_t entry;
} ws_automation_snapshot_iter_t;

typedef enum {
    WS_DEVICE_SNAPSHOT_STAGE_DEVICE = 0,
    WS_DEVICE_SNAPSHOT_STAGE_ENDPOINTS = 1,
    WS_DEVICE_SNAPSHOT_STAGE_STATE = 2,
    WS_DEVICE_SNAPSHOT_STAGE_DONE = 3,
} ws_device_snapshot_stage_t;

typedef struct {
    size_t device_count;
    size_t endpoint_count;
    size_t state_count;
    uint32_t total_records;
    size_t device_index;
    size_t endpoint_index;
    size_t state_index;
    ws_device_snapshot_stage_t stage;
    gw_proto_device_v1_t device;
    gw_proto_state_item_v1_t state_item;
    gw_proto_endpoint_v1_t endpoint;
} ws_device_snapshot_iter_t;

static esp_err_t ws_snapshot_emit_sync_req(void *emit_ctx,
                                           uint8_t msg_type,
                                           uint16_t seq,
                                           const void *payload,
                                           uint16_t payload_len)
{
    httpd_req_t *req = (httpd_req_t *)emit_ctx;
    return ws_send_proto_frame_sync(req, msg_type, seq, payload, payload_len);
}

static bool ws_is_command_type(uint8_t type)
{
    switch (type) {
        case GW_PROTO_MSG_CMD_PERMIT_JOIN:
        case GW_PROTO_MSG_CMD_DEVICE_CHANGE:
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE:
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE_ALL:
        case GW_PROTO_MSG_CMD_GROUP_CREATE:
        case GW_PROTO_MSG_CMD_GROUP_CHANGE:
        case GW_PROTO_MSG_CMD_GROUP_DELETE:
        case GW_PROTO_MSG_CMD_GROUP_ITEMS_CHANGE:
        case GW_PROTO_MSG_CMD_SETTINGS_CHANGE:
        case GW_PROTO_MSG_CMD_AUTOMATION_REMOVE:
        case GW_PROTO_MSG_CMD_AUTOMATION_RESET_ALL:
        case GW_PROTO_MSG_CMD_AUTOMATION_CHANGE:
        case GW_PROTO_MSG_CMD_ACTION_EXEC:
            return true;
        default:
            return false;
    }
}

static esp_err_t ws_route_proto_frame(httpd_req_t *req, int fd, const uint8_t *frame_buf, size_t frame_len)
{
    if (!req || fd <= 0 || !frame_buf || frame_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_hdr_t hdr = {0};
    const uint8_t *payload = NULL;
    esp_err_t err = gw_proto_frame_parse(frame_buf, frame_len, &hdr, &payload);
    if (err != ESP_OK) {
        return err;
    }

    if (hdr.type == GW_PROTO_MSG_SNAPSHOT_REQUEST) {
        return ws_send_proto_snapshot_sync(req, fd);
    }

    err = ws_handle_proto_command(hdr.type, payload, hdr.len);
    if (err != ESP_OK && ws_is_command_type(hdr.type)) {
        ESP_LOGW(TAG,
                 "WS command failed type=0x%02x fd=%d err=%s",
                 (unsigned)hdr.type,
                 fd,
                 esp_err_to_name(err));
        return ESP_OK;
    }

    return err;
}

static esp_err_t ws_remove_all_automations(void)
{
    return gw_model_clear_automations();
}

static uint32_t ws_group_snapshot_total_records(void)
{
    return (uint32_t)(gw_model_count_groups() + gw_model_count_group_items());
}

static uint32_t ws_automation_snapshot_total_records(void)
{
    return (uint32_t)gw_model_count_automations();
}

static uint32_t ws_device_snapshot_total_records(void)
{
    return (uint32_t)(gw_model_count_devices() + gw_model_count_endpoints() + gw_model_count_state());
}

static esp_err_t ws_group_snapshot_rewind(void *source_ctx)
{
    ws_group_snapshot_iter_t *it = (ws_group_snapshot_iter_t *)source_ctx;
    if (!it) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(it, 0, sizeof(*it));
    it->group_count = gw_model_count_groups();
    it->group_item_count = gw_model_count_group_items();
    return ESP_OK;
}

static esp_err_t ws_automation_snapshot_rewind(void *source_ctx)
{
    ws_automation_snapshot_iter_t *it = (ws_automation_snapshot_iter_t *)source_ctx;
    if (!it) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(it, 0, sizeof(*it));
    it->count = gw_model_count_automations();
    return ESP_OK;
}

static esp_err_t ws_device_snapshot_rewind(void *source_ctx)
{
    ws_device_snapshot_iter_t *it = (ws_device_snapshot_iter_t *)source_ctx;
    if (!it) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(it, 0, sizeof(*it));
    it->device_count = gw_model_count_devices();
    it->endpoint_count = gw_model_count_endpoints();
    it->state_count = gw_model_count_state();
    it->total_records = ws_device_snapshot_total_records();
    it->stage = WS_DEVICE_SNAPSHOT_STAGE_DEVICE;
    return ESP_OK;
}

static esp_err_t ws_group_snapshot_next(void *source_ctx,
                                        uint8_t *out_msg_type,
                                        const void **out_payload,
                                        uint16_t *out_payload_len)
{
    ws_group_snapshot_iter_t *it = (ws_group_snapshot_iter_t *)source_ctx;
    if (!it || !out_msg_type || !out_payload || !out_payload_len) {
        return ESP_ERR_INVALID_ARG;
    }

    while (it->group_index < it->group_count) {
            if (gw_model_get_group_by_index(it->group_index++, &it->group) != ESP_OK) {
                continue;
            }
            *out_msg_type = GW_PROTO_MSG_GROUP_UPSERT;
            *out_payload = &it->group;
            *out_payload_len = (uint16_t)sizeof(it->group);
            return ESP_OK;
    }

    while (it->item_index < it->group_item_count) {
        if (gw_model_get_group_item_by_index(it->item_index++, &it->item) != ESP_OK) {
            continue;
        }
        *out_msg_type = GW_PROTO_MSG_GROUP_ITEM_UPSERT;
        *out_payload = &it->item;
        *out_payload_len = (uint16_t)sizeof(it->item);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t ws_automation_snapshot_next(void *source_ctx,
                                             uint8_t *out_msg_type,
                                             const void **out_payload,
                                             uint16_t *out_payload_len)
{
    ws_automation_snapshot_iter_t *it = (ws_automation_snapshot_iter_t *)source_ctx;
    if (!it || !out_msg_type || !out_payload || !out_payload_len) {
        return ESP_ERR_INVALID_ARG;
    }

    while (it->index < it->count) {
        if (gw_model_get_automation_by_index(it->index++, &it->entry) != ESP_OK) {
            continue;
        }
        *out_msg_type = GW_PROTO_MSG_AUTOMATION_UPSERT;
        *out_payload = &it->entry;
        *out_payload_len = (uint16_t)sizeof(it->entry);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t ws_device_snapshot_next(void *source_ctx,
                                         uint8_t *out_msg_type,
                                         const void **out_payload,
                                         uint16_t *out_payload_len)
{
    ws_device_snapshot_iter_t *it = (ws_device_snapshot_iter_t *)source_ctx;
    if (!it || !out_msg_type || !out_payload || !out_payload_len) {
        return ESP_ERR_INVALID_ARG;
    }

    while (it->stage != WS_DEVICE_SNAPSHOT_STAGE_DONE) {
        if (it->stage == WS_DEVICE_SNAPSHOT_STAGE_DEVICE) {
            while (it->device_index < it->device_count) {
                if (gw_model_get_device_by_index(it->device_index++, &it->device) != ESP_OK) {
                    continue;
                }
                *out_msg_type = GW_PROTO_MSG_DEVICE_UPSERT;
                *out_payload = &it->device;
                *out_payload_len = (uint16_t)sizeof(it->device);
                return ESP_OK;
            }
            it->stage = WS_DEVICE_SNAPSHOT_STAGE_ENDPOINTS;
            continue;
        }

        if (it->stage == WS_DEVICE_SNAPSHOT_STAGE_ENDPOINTS) {
            while (it->endpoint_index < it->endpoint_count) {
                if (gw_model_get_endpoint_by_index(it->endpoint_index++, &it->endpoint) != ESP_OK) {
                    continue;
                }
                *out_msg_type = GW_PROTO_MSG_ENDPOINT_UPSERT;
                *out_payload = &it->endpoint;
                *out_payload_len = (uint16_t)sizeof(it->endpoint);
                return ESP_OK;
            }
            it->stage = WS_DEVICE_SNAPSHOT_STAGE_STATE;
            continue;
        }

        if (it->stage == WS_DEVICE_SNAPSHOT_STAGE_STATE) {
            while (it->state_index < it->state_count) {
                if (gw_model_get_state_by_index(it->state_index++, &it->state_item) != ESP_OK) {
                    continue;
                }
                *out_msg_type = GW_PROTO_MSG_STATE_ITEM;
                *out_payload = &it->state_item;
                *out_payload_len = (uint16_t)sizeof(it->state_item);
                return ESP_OK;
            }
            it->stage = WS_DEVICE_SNAPSHOT_STAGE_DONE;
            continue;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t ws_send_proto_groups_snapshot_sync(httpd_req_t *req, uint16_t seq)
{
    ws_group_snapshot_iter_t iter = {0};
    const gw_proto_snapshot_source_t source = {
        .source_ctx = &iter,
        .total_records = ws_group_snapshot_total_records(),
        .rewind = ws_group_snapshot_rewind,
        .next = ws_group_snapshot_next,
    };
    return gw_proto_send_snapshot(ws_snapshot_emit_sync_req,
                                  (void *)req,
                                  GW_PROTO_SYNC_SCOPE_GROUPS,
                                  seq,
                                  &source);
}

static esp_err_t ws_send_proto_automations_snapshot_sync(httpd_req_t *req, uint16_t seq)
{
    ws_automation_snapshot_iter_t iter = {0};
    const gw_proto_snapshot_source_t source = {
        .source_ctx = &iter,
        .total_records = ws_automation_snapshot_total_records(),
        .rewind = ws_automation_snapshot_rewind,
        .next = ws_automation_snapshot_next,
    };
    return gw_proto_send_snapshot(ws_snapshot_emit_sync_req,
                                  (void *)req,
                                  GW_PROTO_SYNC_SCOPE_AUTOMATIONS,
                                  seq,
                                  &source);
}

static esp_err_t ws_send_proto_devices_snapshot_sync(httpd_req_t *req, uint16_t seq)
{
    ws_device_snapshot_iter_t iter = {0};
    esp_err_t err = ws_device_snapshot_rewind(&iter);
    if (err != ESP_OK) {
        return err;
    }
    const gw_proto_snapshot_source_t source = {
        .source_ctx = &iter,
        .total_records = iter.total_records,
        .rewind = NULL,
        .next = ws_device_snapshot_next,
    };
    err = gw_proto_send_snapshot(ws_snapshot_emit_sync_req,
                                 (void *)req,
                                 GW_PROTO_SYNC_SCOPE_DEVICES,
                                 seq,
                                 &source);
    return err;
}

static esp_err_t ws_send_proto_snapshot_sync(httpd_req_t *req, int fd)
{
    (void)fd;
    const uint16_t seq = s_ws_seq++;

    esp_err_t err = ws_send_proto_devices_snapshot_sync(req, seq);
    if (err != ESP_OK) {
        return err;
    }

    err = ws_send_proto_groups_snapshot_sync(req, seq);
    if (err != ESP_OK) {
        return err;
    }

    err = ws_send_proto_automations_snapshot_sync(req, seq);
    if (err != ESP_OK) {
        return err;
    }

    gw_proto_settings_v1_t settings_msg = {0};
    if (gw_model_get_settings(&settings_msg) != ESP_OK) {
        return ESP_OK;
    }

    gw_proto_sync_begin_v1_t settings_begin = {
        .scope = GW_PROTO_SYNC_SCOPE_SETTINGS,
        .reserved0 = 0,
        .reserved1 = 0,
        .total_records = 1,
    };
    err = ws_send_proto_frame_sync(req, GW_PROTO_MSG_SYNC_BEGIN, seq, &settings_begin, sizeof(settings_begin));
    if (err != ESP_OK) {
        return err;
    }

    err = ws_send_proto_frame_sync(req, GW_PROTO_MSG_SETTINGS, seq, &settings_msg, sizeof(settings_msg));
    if (err != ESP_OK) {
        return err;
    }

    gw_proto_sync_end_v1_t settings_end = {
        .scope = GW_PROTO_SYNC_SCOPE_SETTINGS,
        .status = 0,
        .reserved0 = 0,
        .total_records = 1,
    };
    return ws_send_proto_frame_sync(req, GW_PROTO_MSG_SYNC_END, seq, &settings_end, sizeof(settings_end));
}

static size_t ws_collect_client_fds(int *fds, size_t max_fds)
{
    size_t fd_count = 0;
    portENTER_CRITICAL(&s_client_lock);
    ws_prune_stale_clients_locked();
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS && fd_count < max_fds; i++) {
        if (s_clients[i].fd != 0 && s_clients[i].subscribed_events) {
            fds[fd_count++] = s_clients[i].fd;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);
    return fd_count;
}

static void ws_send_proto_trace_to_fds(const gw_proto_trace_v1_t *event, const int *fds, size_t fd_count)
{
    if (!event || !fds || fd_count == 0) {
        return;
    }
    for (size_t i = 0; i < fd_count; i++) {
        (void)ws_send_proto_frame_async(fds[i], GW_PROTO_MSG_EVENT_TRACE, s_ws_seq++, event, sizeof(*event));
    }
}

static void ws_send_proto_model_to_fds(uint8_t type, const void *payload, uint16_t payload_len, const int *fds, size_t fd_count)
{
    if (!payload || !fds || fd_count == 0 || payload_len == 0) {
        return;
    }
    for (size_t i = 0; i < fd_count; i++) {
        (void)ws_send_proto_frame_async(fds[i], type, s_ws_seq++, payload, payload_len);
    }
}

static void ws_on_proto_bus_message(gw_proto_bus_channel_t channel, const gw_proto_hdr_t *hdr, const void *payload, void *user_ctx)
{
    (void)user_ctx;
    if (!hdr || !payload || hdr->len == 0) {
        return;
    }

    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count == 0) {
        return;
    }

    if (channel == GW_PROTO_BUS_CHANNEL_TRACE && hdr->type == GW_PROTO_MSG_EVENT_TRACE) {
        gw_proto_trace_v1_t event = {0};
        const size_t n = hdr->len < sizeof(event) ? hdr->len : sizeof(event);
        memcpy(&event, payload, n);
        ws_send_proto_trace_to_fds(&event, fds, fd_count);
        return;
    }

    ws_send_proto_model_to_fds(hdr->type, payload, hdr->len, fds, fd_count);
}

static void ws_tx_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ws_tx_msg_t msg = {0};
        if (xQueueReceive(s_tx_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.data) {
            if (s_server && msg.fd > 0 &&
                httpd_ws_get_fd_info(s_server, msg.fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_BINARY,
                    .payload = msg.data,
                    .len = msg.len,
                };
                esp_err_t err = httpd_ws_send_data(s_server, msg.fd, &frame);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "WS send failed fd=%d err=%s len=%u", msg.fd, esp_err_to_name(err), (unsigned)msg.len);
                }
            }
            free(msg.data);
        }
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
        uint8_t *buf = NULL;
        if (kWsUsePsram) {
            buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!buf) {
            buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!buf) {
            buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_8BIT);
        }
        if (!buf) return ESP_ERR_NO_MEM;
        frame.payload = buf;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) {
            free(buf);
            return err;
        }
        if (frame.type == HTTPD_WS_TYPE_BINARY) {
            err = ws_route_proto_frame(req, fd, buf, frame.len);
        }
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
    s_tx_q = xQueueCreateStatic(GW_WS_TX_Q_CAP,
                                sizeof(ws_tx_msg_t),
                                s_tx_q_storage,
                                &s_tx_q_struct);
    if (!s_tx_q) {
        s_server = NULL;
        return ESP_ERR_NO_MEM;
    }
    UBaseType_t task_ok = xTaskCreateWithCaps(ws_tx_task_fn, "ws_tx", GW_WS_TX_TASK_STACK, NULL, GW_WS_TX_TASK_PRIO, &s_tx_task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        s_tx_q = NULL;
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
        vTaskDelete(s_tx_task);
        s_tx_task = NULL;
        s_tx_q = NULL;
        s_server = NULL;
        return err;
    }
    (void)gw_proto_bus_add_listener(ws_on_proto_bus_message, GW_PROTO_BUS_CHANNEL_TRACE | GW_PROTO_BUS_CHANNEL_MODEL, NULL);
    ESP_LOGI(TAG, "WebSocket enabled at /ws (gw_proto snapshots/deltas)");
    return ESP_OK;
}

void gw_ws_unregister(void)
{
    if (!s_server) return;

    portENTER_CRITICAL(&s_client_lock);
    memset(s_clients, 0, sizeof(s_clients));
    portEXIT_CRITICAL(&s_client_lock);

    if (s_tx_task) {
        vTaskDelete(s_tx_task);
        s_tx_task = NULL;
    }
    if (s_tx_q) {
        ws_tx_msg_t msg = {0};
        while (xQueueReceive(s_tx_q, &msg, 0) == pdTRUE) {
            free(msg.data);
        }
        s_tx_q = NULL;
    }
    (void)gw_proto_bus_remove_listener(ws_on_proto_bus_message, NULL);
    s_server = NULL;
}




