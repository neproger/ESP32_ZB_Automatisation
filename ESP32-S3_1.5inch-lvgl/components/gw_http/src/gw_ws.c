#include "gw_http/gw_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_core/device_storage.h"
#include "gw_core/action_exec.h"
#include "gw_core/automation_store.h"
#include "gw_core/group_store.h"
#include "gw_core/project_settings.h"
#include "gw_core/state_store.h"
#include "gw_core/zb_model.h"
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

#define GW_WS_MAX_CLIENTS 2
#define GW_WS_TX_Q_CAP 32
#define GW_WS_TX_TASK_PRIO 2
#define GW_WS_TX_TASK_STACK 3072
static gw_ws_client_t s_clients[GW_WS_MAX_CLIENTS];
static QueueHandle_t s_tx_q;
static TaskHandle_t s_tx_task;
static StaticQueue_t s_tx_q_struct;
static uint8_t s_tx_q_storage[GW_WS_TX_Q_CAP * sizeof(ws_tx_msg_t)];
static bool s_device_updates_suppressed;
static esp_timer_handle_t s_device_suppress_timer;

static size_t ws_collect_client_fds(int *fds, size_t max_fds);
static esp_err_t ws_send_binary_async(int fd, const void *buf, size_t len);
static void ws_send_proto_devices_snapshot_to_fds(const int *fds, size_t fd_count);
static void ws_resume_device_updates_work(void *arg);
static void ws_device_suppress_timer_cb(void *arg);
static esp_err_t ws_handle_proto_command(uint8_t type, const uint8_t *payload, uint16_t payload_len);
static void ws_on_state_changed(const gw_state_item_t *item, void *user_ctx);
static void ws_on_device_changed(const gw_device_t *device, bool removed, void *user_ctx);
static void ws_on_endpoint_changed(const gw_zb_endpoint_t *ep, bool removed, void *user_ctx);
static void ws_on_groups_changed(void *user_ctx);
static void ws_on_settings_changed(const gw_project_settings_t *settings, void *user_ctx);
static void ws_on_automations_changed(void *user_ctx);

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
            esp_err_t err = gw_zigbee_permit_join(seconds);
            if (err == ESP_OK) {
                (void)gw_ws_suppress_device_updates(12000);
            }
            return err;
        }
        case GW_PROTO_MSG_CMD_DEVICE_RENAME: {
            if (payload_len < sizeof(gw_proto_cmd_device_rename_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_device_rename_v1_t *msg = (const gw_proto_cmd_device_rename_v1_t *)payload;
            return gw_zigbee_set_device_name(&msg->device_uid, msg->name);
        }
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE: {
            if (payload_len < sizeof(gw_proto_cmd_device_remove_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_device_remove_v1_t *msg = (const gw_proto_cmd_device_remove_v1_t *)payload;
            return gw_zigbee_remove_device(&msg->device_uid);
        }
        case GW_PROTO_MSG_CMD_DEVICE_REMOVE_ALL: {
            if (payload_len < sizeof(gw_proto_cmd_device_remove_all_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            return gw_zigbee_remove_all_devices();
        }
        case GW_PROTO_MSG_CMD_GROUP_CREATE: {
            if (payload_len < sizeof(gw_proto_cmd_group_create_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_create_v1_t *msg = (const gw_proto_cmd_group_create_v1_t *)payload;
            gw_group_entry_t created = {0};
            return gw_group_store_create(msg->id[0] ? msg->id : NULL, msg->name, &created);
        }
        case GW_PROTO_MSG_CMD_GROUP_RENAME: {
            if (payload_len < sizeof(gw_proto_cmd_group_rename_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_rename_v1_t *msg = (const gw_proto_cmd_group_rename_v1_t *)payload;
            return gw_group_store_rename(msg->id, msg->name);
        }
        case GW_PROTO_MSG_CMD_GROUP_DELETE: {
            if (payload_len < sizeof(gw_proto_cmd_group_delete_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_delete_v1_t *msg = (const gw_proto_cmd_group_delete_v1_t *)payload;
            return gw_group_store_remove(msg->id);
        }
        case GW_PROTO_MSG_CMD_GROUP_ITEM_SET: {
            if (payload_len < sizeof(gw_proto_cmd_group_item_set_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_item_set_v1_t *msg = (const gw_proto_cmd_group_item_set_v1_t *)payload;
            return gw_group_store_set_endpoint(msg->group_id, &msg->device_uid, msg->endpoint);
        }
        case GW_PROTO_MSG_CMD_GROUP_ITEM_REMOVE: {
            if (payload_len < sizeof(gw_proto_cmd_group_item_remove_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_item_remove_v1_t *msg = (const gw_proto_cmd_group_item_remove_v1_t *)payload;
            return gw_group_store_remove_endpoint(&msg->device_uid, msg->endpoint);
        }
        case GW_PROTO_MSG_CMD_GROUP_ITEM_REORDER: {
            if (payload_len < sizeof(gw_proto_cmd_group_item_reorder_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_item_reorder_v1_t *msg = (const gw_proto_cmd_group_item_reorder_v1_t *)payload;
            if (msg->order == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_group_store_reorder_endpoint(msg->group_id, &msg->device_uid, msg->endpoint, msg->order);
        }
        case GW_PROTO_MSG_CMD_GROUP_ITEM_LABEL: {
            if (payload_len < sizeof(gw_proto_cmd_group_item_label_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_group_item_label_v1_t *msg = (const gw_proto_cmd_group_item_label_v1_t *)payload;
            return gw_group_store_set_endpoint_label(&msg->device_uid, msg->endpoint, msg->label);
        }
        case GW_PROTO_MSG_CMD_SETTINGS_SET: {
            if (payload_len < sizeof(gw_proto_settings_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_settings_v1_t *msg = (const gw_proto_settings_v1_t *)payload;
            gw_project_settings_t next = {
                .screensaver_timeout_ms = msg->screensaver_timeout_ms,
                .weather_success_interval_ms = msg->weather_success_interval_ms,
                .weather_retry_interval_ms = msg->weather_retry_interval_ms,
                .timezone_auto = (msg->timezone_auto != 0),
                .timezone_offset_min = msg->timezone_offset_min,
            };
            if (!gw_project_settings_validate(&next)) {
                return ESP_ERR_INVALID_ARG;
            }
            return gw_project_settings_set(&next);
        }
        case GW_PROTO_MSG_CMD_AUTOMATION_SET_ENABLED: {
            if (payload_len < sizeof(gw_proto_cmd_automation_set_enabled_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_automation_set_enabled_v1_t *msg = (const gw_proto_cmd_automation_set_enabled_v1_t *)payload;
            return gw_automation_store_set_enabled(msg->id, msg->enabled != 0);
        }
        case GW_PROTO_MSG_CMD_AUTOMATION_REMOVE: {
            if (payload_len < sizeof(gw_proto_cmd_automation_remove_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_proto_cmd_automation_remove_v1_t *msg = (const gw_proto_cmd_automation_remove_v1_t *)payload;
            return gw_automation_store_remove(msg->id);
        }
        case GW_PROTO_MSG_CMD_AUTOMATION_RESET_ALL: {
            if (payload_len < sizeof(gw_proto_cmd_automation_reset_all_v1_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            return gw_automation_store_remove_all();
        }
        case GW_PROTO_MSG_CMD_AUTOMATION_SAVE: {
            if (payload_len < sizeof(gw_automation_entry_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_automation_entry_t *msg = (const gw_automation_entry_t *)payload;
            return gw_automation_store_put_entry(msg);
        }
        case GW_PROTO_MSG_CMD_ACTION_EXEC: {
            if (payload_len < sizeof(gw_automation_entry_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            const gw_automation_entry_t *msg = (const gw_automation_entry_t *)payload;
            char errbuf[128] = {0};
            return gw_action_exec_entry(msg, errbuf, sizeof(errbuf));
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

typedef struct {
    const int *fds;
    size_t fd_count;
} ws_proto_emit_many_ctx_t;

typedef struct {
    size_t group_count;
    size_t group_index;
    size_t item_index;
    bool emit_group;
    gw_group_entry_t group;
    gw_group_item_t item;
    gw_proto_group_v1_t group_msg;
    gw_proto_group_item_v1_t item_msg;
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
    size_t state_count;
    uint32_t total_records;
    size_t device_index;
    size_t endpoint_index;
    size_t endpoint_count;
    size_t state_index;
    ws_device_snapshot_stage_t stage;
    gw_device_full_t device;
    gw_state_item_t state_item;
    gw_device_t device_view;
    gw_zb_endpoint_t endpoint_view;
    gw_proto_device_v1_t device_msg;
    gw_proto_endpoint_v1_t endpoint_msg;
    gw_proto_state_item_v1_t state_msg;
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

static esp_err_t ws_snapshot_emit_many(void *emit_ctx,
                                       uint8_t msg_type,
                                       uint16_t seq,
                                       const void *payload,
                                       uint16_t payload_len)
{
    const ws_proto_emit_many_ctx_t *ctx = (const ws_proto_emit_many_ctx_t *)emit_ctx;
    if (!ctx || !ctx->fds || ctx->fd_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < ctx->fd_count; i++) {
        esp_err_t err = ws_send_proto_frame_async(ctx->fds[i], msg_type, seq, payload, payload_len);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static uint32_t ws_group_snapshot_total_records(void)
{
    const size_t group_count = gw_group_store_count();
    size_t group_item_count = 0;
    for (size_t i = 0; i < group_count; i++) {
        gw_group_entry_t group = {0};
        if (gw_group_store_get_by_index(i, &group) != ESP_OK) {
            continue;
        }
        group_item_count += gw_group_store_get_group_item_count(group.id);
    }
    return (uint32_t)(group_count + group_item_count);
}

static uint32_t ws_automation_snapshot_total_records(void)
{
    return (uint32_t)gw_automation_store_count();
}

static uint32_t ws_device_snapshot_total_records(void)
{
    const size_t device_count = gw_device_storage_count();
    const size_t state_count = gw_state_store_count();
    size_t endpoint_count = 0;

    for (size_t i = 0; i < device_count; i++) {
        gw_device_full_t dev = {0};
        if (gw_device_storage_get_by_index(i, &dev) != ESP_OK) {
            continue;
        }
        endpoint_count += gw_zb_model_list_endpoints(&dev.device_uid, NULL, 0);
    }

    return (uint32_t)(device_count + endpoint_count + state_count);
}

static esp_err_t ws_group_snapshot_rewind(void *source_ctx)
{
    ws_group_snapshot_iter_t *it = (ws_group_snapshot_iter_t *)source_ctx;
    if (!it) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(it, 0, sizeof(*it));
    it->group_count = gw_group_store_count();
    it->emit_group = true;
    return ESP_OK;
}

static esp_err_t ws_automation_snapshot_rewind(void *source_ctx)
{
    ws_automation_snapshot_iter_t *it = (ws_automation_snapshot_iter_t *)source_ctx;
    if (!it) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(it, 0, sizeof(*it));
    it->count = gw_automation_store_count();
    return ESP_OK;
}

static esp_err_t ws_device_snapshot_rewind(void *source_ctx)
{
    ws_device_snapshot_iter_t *it = (ws_device_snapshot_iter_t *)source_ctx;
    if (!it) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(it, 0, sizeof(*it));
    it->device_count = gw_device_storage_count();
    it->state_count = gw_state_store_count();
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
        if (it->emit_group) {
            if (gw_group_store_get_by_index(it->group_index, &it->group) != ESP_OK) {
                it->group_index++;
                it->item_index = 0;
                it->emit_group = true;
                continue;
            }
            gw_proto_fill_group(&it->group_msg, &it->group);
            it->emit_group = false;
            *out_msg_type = GW_PROTO_MSG_GROUP_UPSERT;
            *out_payload = &it->group_msg;
            *out_payload_len = (uint16_t)sizeof(it->group_msg);
            return ESP_OK;
        }

        const size_t item_count = gw_group_store_get_group_item_count(it->group.id);
        while (it->item_index < item_count) {
            if (gw_group_store_get_group_item_by_index(it->group.id, it->item_index, &it->item) != ESP_OK) {
                it->item_index++;
                continue;
            }
            it->item_index++;
            gw_proto_fill_group_item(&it->item_msg, &it->item);
            *out_msg_type = GW_PROTO_MSG_GROUP_ITEM_UPSERT;
            *out_payload = &it->item_msg;
            *out_payload_len = (uint16_t)sizeof(it->item_msg);
            return ESP_OK;
        }

        it->group_index++;
        it->item_index = 0;
        it->emit_group = true;
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
        if (gw_automation_store_get_by_index(it->index++, &it->entry) != ESP_OK) {
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
                if (gw_device_storage_get_by_index(it->device_index, &it->device) != ESP_OK) {
                    it->device_index++;
                    continue;
                }

                memset(&it->device_view, 0, sizeof(it->device_view));
                it->device_view.device_uid = it->device.device_uid;
                it->device_view.short_addr = it->device.short_addr;
                it->device_view.version = it->device.version;
                it->device_view.last_seen_ms = it->device.last_seen_ms;
                it->device_view.has_onoff = it->device.has_onoff;
                it->device_view.has_button = it->device.has_button;
                strlcpy(it->device_view.name, it->device.name, sizeof(it->device_view.name));

                gw_proto_fill_device(&it->device_msg, &it->device_view);
                it->endpoint_count = gw_zb_model_list_endpoints(&it->device.device_uid, NULL, 0);
                it->endpoint_index = 0;
                it->stage = WS_DEVICE_SNAPSHOT_STAGE_ENDPOINTS;
                it->device_index++;
                *out_msg_type = GW_PROTO_MSG_DEVICE_UPSERT;
                *out_payload = &it->device_msg;
                *out_payload_len = (uint16_t)sizeof(it->device_msg);
                return ESP_OK;
            }
            it->stage = WS_DEVICE_SNAPSHOT_STAGE_STATE;
            continue;
        }

        if (it->stage == WS_DEVICE_SNAPSHOT_STAGE_ENDPOINTS) {
            while (it->endpoint_index < it->endpoint_count && it->endpoint_index < GW_ZB_MAX_ENDPOINTS) {
                if (gw_zb_model_get_endpoint_by_index(&it->device.device_uid,
                                                      it->endpoint_index,
                                                      &it->endpoint_view) != ESP_OK) {
                    it->endpoint_index++;
                    continue;
                }
                it->endpoint_index++;

                if (it->endpoint_view.endpoint == 0 ||
                    (it->endpoint_view.profile_id == 0 && it->endpoint_view.device_id == 0 &&
                     it->endpoint_view.in_cluster_count == 0 && it->endpoint_view.out_cluster_count == 0)) {
                    continue;
                }

                it->endpoint_view.version = it->device.version;

                gw_proto_fill_endpoint(&it->endpoint_msg, &it->endpoint_view);
                *out_msg_type = GW_PROTO_MSG_ENDPOINT_UPSERT;
                *out_payload = &it->endpoint_msg;
                *out_payload_len = (uint16_t)sizeof(it->endpoint_msg);
                return ESP_OK;
            }
            it->stage = WS_DEVICE_SNAPSHOT_STAGE_DEVICE;
            continue;
        }

        if (it->stage == WS_DEVICE_SNAPSHOT_STAGE_STATE) {
            while (it->state_index < it->state_count) {
                if (gw_state_store_get_by_index(it->state_index++, &it->state_item) != ESP_OK) {
                    continue;
                }
                gw_proto_fill_state_item(&it->state_msg, &it->state_item);
                *out_msg_type = GW_PROTO_MSG_STATE_ITEM;
                *out_payload = &it->state_msg;
                *out_payload_len = (uint16_t)sizeof(it->state_msg);
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

static void ws_send_proto_devices_snapshot_to_fds(const int *fds, size_t fd_count)
{
    if (!fds || fd_count == 0) {
        return;
    }
    ws_device_snapshot_iter_t iter = {0};
    if (ws_device_snapshot_rewind(&iter) != ESP_OK) {
        return;
    }
    const ws_proto_emit_many_ctx_t ctx = {
        .fds = fds,
        .fd_count = fd_count,
    };
    const gw_proto_snapshot_source_t source = {
        .source_ctx = &iter,
        .total_records = iter.total_records,
        .rewind = NULL,
        .next = ws_device_snapshot_next,
    };
    (void)gw_proto_send_snapshot(ws_snapshot_emit_many,
                                 (void *)&ctx,
                                 GW_PROTO_SYNC_SCOPE_DEVICES,
                                 s_ws_seq++,
                                 &source);
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

    gw_project_settings_t settings = {0};
    if (gw_project_settings_get(&settings) != ESP_OK) {
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

    gw_proto_settings_v1_t settings_msg = {0};
    gw_proto_fill_settings(&settings_msg, &settings);
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

static void ws_resume_device_updates_work(void *arg)
{
    (void)arg;
    s_device_updates_suppressed = false;
    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count == 0) {
        return;
    }
    ESP_LOGI(TAG, "WS device updates resumed; sending fresh devices snapshot");
    ws_send_proto_devices_snapshot_to_fds(fds, fd_count);
}

static void ws_device_suppress_timer_cb(void *arg)
{
    (void)arg;
    if (!s_server) {
        s_device_updates_suppressed = false;
        return;
    }
    if (httpd_queue_work(s_server, ws_resume_device_updates_work, NULL) != ESP_OK) {
        s_device_updates_suppressed = false;
    }
}

static void ws_send_proto_groups_snapshot_to_fds(const int *fds, size_t fd_count)
{
    if (!fds || fd_count == 0) {
        return;
    }

    const uint16_t seq = s_ws_seq++;
    ws_group_snapshot_iter_t iter = {0};
    const ws_proto_emit_many_ctx_t emit_ctx = {
        .fds = fds,
        .fd_count = fd_count,
    };
    const gw_proto_snapshot_source_t source = {
        .source_ctx = &iter,
        .total_records = ws_group_snapshot_total_records(),
        .rewind = ws_group_snapshot_rewind,
        .next = ws_group_snapshot_next,
    };
    (void)gw_proto_send_snapshot(ws_snapshot_emit_many,
                                 (void *)&emit_ctx,
                                 GW_PROTO_SYNC_SCOPE_GROUPS,
                                 seq,
                                 &source);
}

static void ws_send_proto_automations_snapshot_to_fds(const int *fds, size_t fd_count)
{
    if (!fds || fd_count == 0) {
        return;
    }

    const uint16_t seq = s_ws_seq++;
    ws_automation_snapshot_iter_t iter = {0};
    const ws_proto_emit_many_ctx_t emit_ctx = {
        .fds = fds,
        .fd_count = fd_count,
    };
    const gw_proto_snapshot_source_t source = {
        .source_ctx = &iter,
        .total_records = ws_automation_snapshot_total_records(),
        .rewind = ws_automation_snapshot_rewind,
        .next = ws_automation_snapshot_next,
    };
    (void)gw_proto_send_snapshot(ws_snapshot_emit_many,
                                 (void *)&emit_ctx,
                                 GW_PROTO_SYNC_SCOPE_AUTOMATIONS,
                                 seq,
                                 &source);
}

static void ws_send_proto_settings_to_fds(const int *fds, size_t fd_count)
{
    if (!fds || fd_count == 0) {
        return;
    }

    gw_project_settings_t settings = {0};
    if (gw_project_settings_get(&settings) != ESP_OK) {
        return;
    }

    gw_proto_settings_v1_t msg = {0};
    gw_proto_fill_settings(&msg, &settings);
    for (size_t i = 0; i < fd_count; i++) {
        (void)ws_send_proto_frame_async(fds[i], GW_PROTO_MSG_SETTINGS, s_ws_seq++, &msg, sizeof(msg));
    }
}

static size_t ws_collect_client_fds(int *fds, size_t max_fds)
{
    size_t fd_count = 0;
    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS && fd_count < max_fds; i++) {
        if (s_clients[i].fd != 0 && s_clients[i].subscribed_events) {
            fds[fd_count++] = s_clients[i].fd;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);
    return fd_count;
}

static void ws_send_proto_state_item_to_fds(const gw_state_item_t *item, const int *fds, size_t fd_count)
{
    if (!item || !fds || fd_count == 0 || item->uid.uid[0] == '\0') {
        return;
    }

    gw_proto_state_item_v1_t msg = {0};
    gw_proto_fill_state_item(&msg, item);
    for (size_t i = 0; i < fd_count; i++) {
        (void)ws_send_proto_frame_async(fds[i], GW_PROTO_MSG_STATE_ITEM, s_ws_seq++, &msg, sizeof(msg));
    }
}

static void ws_send_proto_device_delta_to_fds(const gw_device_t *device, bool remove_only, const int *fds, size_t fd_count)
{
    if (!device || device->device_uid.uid[0] == '\0' || !fds || fd_count == 0) {
        return;
    }

    if (remove_only) {
        gw_proto_device_remove_v1_t rm = {0};
        gw_proto_fill_device_remove(&rm, &device->device_uid);
        for (size_t i = 0; i < fd_count; i++) {
            (void)ws_send_proto_frame_async(fds[i], GW_PROTO_MSG_DEVICE_REMOVE, s_ws_seq++, &rm, sizeof(rm));
        }
        return;
    }

    gw_proto_device_v1_t msg = {0};
    gw_proto_fill_device(&msg, device);
    for (size_t i = 0; i < fd_count; i++) {
        (void)ws_send_proto_frame_async(fds[i], GW_PROTO_MSG_DEVICE_UPSERT, s_ws_seq++, &msg, sizeof(msg));
    }
}

static void ws_on_state_changed(const gw_state_item_t *item, void *user_ctx)
{
    (void)user_ctx;
    if (s_device_updates_suppressed) {
        return;
    }
    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count > 0) {
        ws_send_proto_state_item_to_fds(item, fds, fd_count);
    }
}

static void ws_on_device_changed(const gw_device_t *device, bool removed, void *user_ctx)
{
    (void)user_ctx;
    if (s_device_updates_suppressed) {
        return;
    }
    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count > 0) {
        ws_send_proto_device_delta_to_fds(device, removed, fds, fd_count);
    }
}

static void ws_on_endpoint_changed(const gw_zb_endpoint_t *ep, bool removed, void *user_ctx)
{
    (void)user_ctx;
    if (!ep || s_device_updates_suppressed) {
        return;
    }
    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count == 0) {
        return;
    }
    if (removed) {
        gw_proto_endpoint_remove_v1_t rm = {0};
        gw_proto_fill_endpoint_remove(&rm, &ep->uid, ep->endpoint, ep->short_addr);
        for (size_t i = 0; i < fd_count; i++) {
            (void)ws_send_proto_frame_async(fds[i], GW_PROTO_MSG_ENDPOINT_REMOVE, s_ws_seq++, &rm, sizeof(rm));
        }
        return;
    }
    gw_proto_endpoint_v1_t msg = {0};
    gw_proto_fill_endpoint(&msg, ep);
    for (size_t i = 0; i < fd_count; i++) {
        (void)ws_send_proto_frame_async(fds[i], GW_PROTO_MSG_ENDPOINT_UPSERT, s_ws_seq++, &msg, sizeof(msg));
    }
}

static void ws_on_groups_changed(void *user_ctx)
{
    (void)user_ctx;
    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count > 0) {
        ws_send_proto_groups_snapshot_to_fds(fds, fd_count);
    }
}

static void ws_on_settings_changed(const gw_project_settings_t *settings, void *user_ctx)
{
    (void)settings;
    (void)user_ctx;
    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count > 0) {
        ws_send_proto_settings_to_fds(fds, fd_count);
    }
}

static void ws_on_automations_changed(void *user_ctx)
{
    (void)user_ctx;
    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = ws_collect_client_fds(fds, GW_WS_MAX_CLIENTS);
    if (fd_count > 0) {
        ws_send_proto_automations_snapshot_to_fds(fds, fd_count);
    }
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
            gw_proto_hdr_t hdr = {0};
            const uint8_t *payload = NULL;
            err = gw_proto_frame_parse(buf, frame.len, &hdr, &payload);
            if (err == ESP_OK) {
                if (hdr.type == GW_PROTO_MSG_SNAPSHOT_REQUEST) {
                    err = ws_send_proto_snapshot_sync(req, fd);
                } else {
                    err = ws_handle_proto_command(hdr.type, payload, hdr.len);
                }
            }
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
    s_device_updates_suppressed = false;
    s_device_suppress_timer = NULL;
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
    const esp_timer_create_args_t suppress_timer_args = {
        .callback = ws_device_suppress_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ws_suppress",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&suppress_timer_args, &s_device_suppress_timer) != ESP_OK) {
        vTaskDelete(s_tx_task);
        s_tx_task = NULL;
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
        esp_timer_delete(s_device_suppress_timer);
        s_device_suppress_timer = NULL;
        vTaskDelete(s_tx_task);
        s_tx_task = NULL;
        s_tx_q = NULL;
        s_server = NULL;
        return err;
    }
    (void)gw_state_store_add_listener(ws_on_state_changed, NULL);
    (void)gw_device_registry_add_listener(ws_on_device_changed, NULL);
    (void)gw_zb_model_add_listener(ws_on_endpoint_changed, NULL);
    (void)gw_group_store_add_listener(ws_on_groups_changed, NULL);
    (void)gw_project_settings_add_listener(ws_on_settings_changed, NULL);
    (void)gw_automation_store_add_listener(ws_on_automations_changed, NULL);
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
    if (s_device_suppress_timer) {
        esp_timer_stop(s_device_suppress_timer);
        esp_timer_delete(s_device_suppress_timer);
        s_device_suppress_timer = NULL;
    }
    if (s_tx_q) {
        ws_tx_msg_t msg = {0};
        while (xQueueReceive(s_tx_q, &msg, 0) == pdTRUE) {
            free(msg.data);
        }
        s_tx_q = NULL;
    }
    (void)gw_state_store_remove_listener(ws_on_state_changed, NULL);
    (void)gw_device_registry_remove_listener(ws_on_device_changed, NULL);
    (void)gw_zb_model_remove_listener(ws_on_endpoint_changed, NULL);
    (void)gw_group_store_remove_listener(ws_on_groups_changed, NULL);
    (void)gw_project_settings_remove_listener(ws_on_settings_changed, NULL);
    (void)gw_automation_store_remove_listener(ws_on_automations_changed, NULL);
    s_server = NULL;
}

esp_err_t gw_ws_suppress_device_updates(uint32_t duration_ms)
{
    if (!s_server || !s_device_suppress_timer || duration_ms == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_device_updates_suppressed = true;
    (void)esp_timer_stop(s_device_suppress_timer);
    esp_err_t err = esp_timer_start_once(s_device_suppress_timer, (uint64_t)duration_ms * 1000ULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WS device updates suppressed for %u ms", (unsigned)duration_ms);
    }
    return err;
}




