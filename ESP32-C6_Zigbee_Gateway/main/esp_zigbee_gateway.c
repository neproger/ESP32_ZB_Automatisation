/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * Zigbee Gateway Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include <fcntl.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "esp_vfs_eventfd.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_zigbee_gateway.h"
#include "esp_zigbee_cluster.h"
#include "zb_config_platform.h"
#include "zboss_api.h"

#include "gw_zigbee/gw_zigbee.h"
#include "gw_core/deleted_devices.h"
#include "gw_core/device_registry.h"
#include "gw_core/gw_proto.h"
#include "gw_core/zb_model.h"
#include "gw_uart_link.h"

#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_core.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"

static const char *TAG = "ESP_ZB_GATEWAY";

#define GW_TASK_PRIO_ZIGBEE 8

#define GW_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT 0x0400
#define GW_ZB_CLUSTER_PRESSURE_MEASUREMENT    0x0403
#define GW_ZB_CLUSTER_OCCUPANCY_SENSING       0x0406
#define GW_ZB_ATTR_MEASURED_VALUE             0x0000
#define GW_ZB_ATTR_OCCUPANCY                  0x0000
#define GW_ZB_ATTR_BATTERY_VOLTAGE            0x0020

static void uart_send_zb_event(uint8_t event_kind,
                               const gw_device_uid_t *uid,
                               uint16_t short_addr,
                               uint8_t endpoint,
                               uint16_t cluster_id,
                               uint16_t attr_id,
                               uint8_t value_type,
                               bool value_bool,
                               int64_t value_i64,
                               float value_f32,
                               const char *cmd,
                               const char *value_text)
{
    gw_proto_event_v1_t evt = {0};
    evt.event_id = 0;
    evt.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    evt.event_id_kind = event_kind;
    if (uid) {
        evt.device_uid = *uid;
    }
    evt.short_addr = short_addr;
    evt.endpoint = endpoint;
    evt.cluster_id = cluster_id;
    evt.attr_id = attr_id;
    evt.value_type = value_type;
    evt.value_bool = value_bool ? 1u : 0u;
    evt.value_i64 = value_i64;
    evt.value_f32 = value_f32;
    if (cmd) {
        strlcpy(evt.cmd, cmd, sizeof(evt.cmd));
    }
    if (value_text) {
        strlcpy(evt.value_text, value_text, sizeof(evt.value_text));
    }
    (void)gw_uart_link_send_event_zb(&evt);
}

static void uart_send_net_state_online(void)
{
    gw_proto_event_v1_t evt = {0};
    evt.event_id = 0;
    evt.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    evt.event_id_kind = GW_PROTO_EVENT_NET_STATE;
    strlcpy(evt.cmd, "online", sizeof(evt.cmd));
    strlcpy(evt.value_text, "c6_boot", sizeof(evt.value_text));
    (void)gw_uart_link_send_event_zb(&evt);
}
static bool s_snapshot_runtime_ready_sent;
static bool s_addr_table_dump_done;

static void ieee_to_uid_string(const esp_zb_ieee_addr_t ieee, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (ieee == NULL) {
        out[0] = '\0';
        return;
    }
    (void)snprintf(out,
                   out_len,
                   "0x%02x%02x%02x%02x%02x%02x%02x%02x",
                   ieee[0], ieee[1], ieee[2], ieee[3],
                   ieee[4], ieee[5], ieee[6], ieee[7]);
}

static void dump_address_table_once(void)
{
    if (s_addr_table_dump_done) {
        return;
    }
    s_addr_table_dump_done = true;

    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        ESP_LOGW(TAG, "addr table dump skipped: zigbee lock unavailable");
        return;
    }

    const zb_ushort_t max_scan = 256U;
    ESP_LOGW(TAG, "=== [ZB ADDRESS TABLE DUMP] scan_max=%u ===", (unsigned)max_scan);
    for (zb_ushort_t i = 0; i < max_scan; ++i) {
        zb_address_ieee_ref_t ref = 0;
        if (zb_address_by_sorted_table_index(i, &ref) != RET_OK) {
            continue;
        }
        if (!zb_address_in_use(ref)) {
            continue;
        }

        zb_ieee_addr_t ieee_addr = {0};
        uint8_t raw_ieee[8] = {0};
        zb_uint16_t short_addr = ZB_UNKNOWN_SHORT_ADDR;
        zb_address_ieee_by_ref(ieee_addr, ref);
        zb_address_short_by_ref(&short_addr, ref);
        memcpy(raw_ieee, ieee_addr, sizeof(raw_ieee));

        bool is_zero = true;
        for (int b = 0; b < (int)sizeof(raw_ieee); ++b) {
            if (raw_ieee[b] != 0) {
                is_zero = false;
                break;
            }
        }
        if (is_zero) {
            continue;
        }

        ESP_LOGW(TAG,
                 "addr[%02u] ref=%u ieee=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x nwk=0x%04x",
                 (unsigned)i,
                 (unsigned)ref,
                 raw_ieee[7], raw_ieee[6], raw_ieee[5], raw_ieee[4],
                 raw_ieee[3], raw_ieee[2], raw_ieee[1], raw_ieee[0],
                 (unsigned)short_addr);
    }
    ESP_LOGW(TAG, "=== [ZB ADDRESS TABLE DUMP END] ===");
    esp_zb_lock_release();
}

static void announce_snapshot_runtime_ready_once(void)
{
    if (s_snapshot_runtime_ready_sent) {
        return;
    }
    s_snapshot_runtime_ready_sent = true;
    gw_uart_link_set_snapshot_ready(true);
    uart_send_net_state_online();
    ESP_LOGI(TAG, "startup topology bootstrap ready");
    dump_address_table_once();
}

static void touch_device_last_seen(const gw_device_uid_t *uid, uint16_t short_addr, uint64_t ts_ms)
{
    if (!uid || uid->uid[0] == '\0') {
        return;
    }

    gw_device_t d = {0};
    if (gw_device_registry_get(uid, &d) != ESP_OK) {
        return;
    }
    d.short_addr = short_addr;
    d.last_seen_ms = ts_ms;
    (void)gw_device_registry_upsert(&d);
}

static const char *zb_cmd_name(uint16_t cluster_id, uint8_t cmd_id)
{
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        switch (cmd_id) {
        case ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID: return "off";
        case ESP_ZB_ZCL_CMD_ON_OFF_ON_ID: return "on";
        case ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID: return "toggle";
        case ESP_ZB_ZCL_CMD_ON_OFF_OFF_WITH_EFFECT_ID: return "off_effect";
        case ESP_ZB_ZCL_CMD_ON_OFF_ON_WITH_RECALL_GLOBAL_SCENE_ID: return "on_recall";
        case ESP_ZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID: return "on_timed";
        default: return "onoff_unknown";
        }
    }
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
        switch (cmd_id) {
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE_TO_LEVEL: return "move_to_level";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE: return "move";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STEP: return "step";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STOP: return "stop";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE_TO_LEVEL_WITH_ON_OFF: return "mv_lvl_onoff";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE_WITH_ON_OFF: return "move_onoff";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STEP_WITH_ON_OFF: return "step_onoff";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STOP_WITH_ON_OFF: return "stop_onoff";
        case ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE_TO_CLOSEST_FREQUENCY: return "move_freq";
        default: return "level_unknown";
        }
    }
    return "unknown";
}

static esp_err_t zb_core_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id == ESP_ZB_CORE_REPORT_ATTR_CB_ID) {
        const esp_zb_zcl_report_attr_message_t *m = (const esp_zb_zcl_report_attr_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }

        uint16_t src_short = 0;
        if (m->src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
            src_short = m->src_address.u.short_addr;
        }

        gw_device_uid_t uid = {0};
        if (!gw_zb_model_find_uid_by_short(src_short, &uid) && src_short != 0) {
            ESP_LOGW(TAG,
                     "attr report from unknown short=0x%04x ep=%u cluster=0x%04x attr=0x%04x, scheduling discovery",
                     (unsigned)src_short,
                     (unsigned)m->src_endpoint,
                     (unsigned)m->cluster,
                     (unsigned)m->attribute.id);
            (void)gw_zigbee_discover_by_short(src_short);
        }

        const uint16_t cluster_id = m->cluster;
        const uint16_t attr_id = m->attribute.id;
        if (uid.uid[0] != '\0' && m->attribute.data.value != NULL) {
            const uint64_t ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
            touch_device_last_seen(&uid, src_short, ts_ms);
        }

        // Normalized event: zigbee.attr_report (msg + structured payload)
        {
            gw_proto_event_value_type_t vtype = GW_PROTO_EVENT_VALUE_NONE;
            bool vbool = false;
            int64_t vi64 = 0;
            double vf64 = 0.0;
            const char *vtext = NULL;
            if (m->attribute.data.value != NULL) {
                if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
                    (m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL || m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) &&
                    m->attribute.data.size >= 1) {
                    vtype = GW_PROTO_EVENT_VALUE_BOOL;
                    vbool = (*((const uint8_t *)m->attribute.data.value) != 0);
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT &&
                           attr_id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16 && m->attribute.data.size >= 2) {
                    vtype = GW_PROTO_EVENT_VALUE_F32;
                    vf64 = ((double)(*((const int16_t *)m->attribute.data.value))) / 100.0;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT &&
                           attr_id == ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && m->attribute.data.size >= 2) {
                    vtype = GW_PROTO_EVENT_VALUE_F32;
                    vf64 = ((double)(*((const uint16_t *)m->attribute.data.value))) / 100.0;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                           attr_id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && m->attribute.data.size >= 1) {
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)(*((const uint8_t *)m->attribute.data.value) / 2u);
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                           attr_id == GW_ZB_ATTR_BATTERY_VOLTAGE &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && m->attribute.data.size >= 1) {
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)(*((const uint8_t *)m->attribute.data.value)) * 100;
                } else if (cluster_id == GW_ZB_CLUSTER_OCCUPANCY_SENSING &&
                           attr_id == GW_ZB_ATTR_OCCUPANCY &&
                           (m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_8BITMAP || m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) &&
                           m->attribute.data.size >= 1) {
                    vtype = GW_PROTO_EVENT_VALUE_BOOL;
                    vbool = ((*((const uint8_t *)m->attribute.data.value) & 0x01u) != 0);
                } else if (cluster_id == GW_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT &&
                           attr_id == GW_ZB_ATTR_MEASURED_VALUE &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && m->attribute.data.size >= 2) {
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)(*((const uint16_t *)m->attribute.data.value));
                } else if (cluster_id == GW_ZB_CLUSTER_PRESSURE_MEASUREMENT &&
                           attr_id == GW_ZB_ATTR_MEASURED_VALUE &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16 && m->attribute.data.size >= 2) {
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)(*((const int16_t *)m->attribute.data.value));
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
                           attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && m->attribute.data.size >= 1) {
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)(*((const uint8_t *)m->attribute.data.value));
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
                           m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && m->attribute.data.size >= 2 &&
                           (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID ||
                            attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID ||
                            attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID)) {
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)(*((const uint16_t *)m->attribute.data.value));
                }
            }
            uart_send_zb_event(GW_PROTO_EVENT_ATTR_REPORT,
                               &uid,
                               src_short,
                               m->src_endpoint,
                               cluster_id,
                               attr_id,
                               (uint8_t)vtype,
                               vbool,
                               vi64,
                               (float)vf64,
                               NULL,
                               vtext);
        }

        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *m = (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }

        uint16_t src_short = 0;
        if (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
            src_short = m->info.src_address.u.short_addr;
        }

        gw_device_uid_t uid = {0};
        if (!gw_zb_model_find_uid_by_short(src_short, &uid) && src_short != 0) {
            ESP_LOGW(TAG,
                     "read attr response from unknown short=0x%04x ep=%u cluster=0x%04x, scheduling discovery",
                     (unsigned)src_short,
                     (unsigned)m->info.src_endpoint,
                     (unsigned)m->info.cluster);
            (void)gw_zigbee_discover_by_short(src_short);
        }

        for (esp_zb_zcl_read_attr_resp_variable_t *it = m->variables; it != NULL; it = it->next) {
            const uint16_t cluster_id = m->info.cluster;
            const uint16_t attr_id = it->attribute.id;
            gw_proto_event_value_type_t vtype = GW_PROTO_EVENT_VALUE_NONE;
            bool vbool = false;
            int64_t vi64 = 0;
            double vf64 = 0.0;
            bool has_state_update = false;

            if (uid.uid[0] != '\0' && it->status == ESP_ZB_ZCL_STATUS_SUCCESS && it->attribute.data.value != NULL) {
                const uint64_t ts_ms = (uint64_t)(esp_timer_get_time() / 1000);

                if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr_id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID &&
                    it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16 && it->attribute.data.size >= 2) {
                    const int16_t raw = *((const int16_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_F32;
                    vf64 = ((double)raw) / 100.0;
                    has_state_update = true;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT &&
                           attr_id == ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID && it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 &&
                           it->attribute.data.size >= 2) {
                    const uint16_t raw = *((const uint16_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_F32;
                    vf64 = ((double)raw) / 100.0;
                    has_state_update = true;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                           attr_id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID && it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 &&
                           it->attribute.data.size >= 1) {
                    const uint8_t raw = *((const uint8_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)(raw / 2u);
                    has_state_update = true;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
                           attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
                           (it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL || it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) &&
                           it->attribute.data.size >= 1) {
                    uint8_t onoff = *((const uint8_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_BOOL;
                    vbool = (onoff != 0);
                    has_state_update = true;
                } else if (cluster_id == GW_ZB_CLUSTER_OCCUPANCY_SENSING &&
                           attr_id == GW_ZB_ATTR_OCCUPANCY &&
                           (it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_8BITMAP || it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) &&
                           it->attribute.data.size >= 1) {
                    uint8_t occ = *((const uint8_t *)it->attribute.data.value);
                    bool occupied = ((occ & 0x01u) != 0);
                    vtype = GW_PROTO_EVENT_VALUE_BOOL;
                    vbool = occupied;
                    has_state_update = true;
                } else if (cluster_id == GW_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT &&
                           attr_id == GW_ZB_ATTR_MEASURED_VALUE &&
                           it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 &&
                           it->attribute.data.size >= 2) {
                    const uint16_t raw = *((const uint16_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)raw;
                    has_state_update = true;
                } else if (cluster_id == GW_ZB_CLUSTER_PRESSURE_MEASUREMENT &&
                           attr_id == GW_ZB_ATTR_MEASURED_VALUE &&
                           it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16 &&
                           it->attribute.data.size >= 2) {
                    const int16_t raw = *((const int16_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)raw;
                    has_state_update = true;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                           attr_id == GW_ZB_ATTR_BATTERY_VOLTAGE &&
                           it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 &&
                           it->attribute.data.size >= 1) {
                    uint32_t mv = (uint32_t)(*((const uint8_t *)it->attribute.data.value)) * 100u;
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)mv;
                    has_state_update = true;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
                           attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID &&
                           it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 &&
                           it->attribute.data.size >= 1) {
                    const uint8_t raw = *((const uint8_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)raw;
                    has_state_update = true;
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
                           it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 &&
                           it->attribute.data.size >= 2 &&
                           (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID ||
                            attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID ||
                            attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID)) {
                    const uint16_t raw = *((const uint16_t *)it->attribute.data.value);
                    vtype = GW_PROTO_EVENT_VALUE_I64;
                    vi64 = (int64_t)raw;
                    has_state_update = true;
                }

                if (has_state_update) {
                    touch_device_last_seen(&uid, src_short, ts_ms);
                    uart_send_zb_event(GW_PROTO_EVENT_ATTR_REPORT,
                                       &uid,
                                       src_short,
                                       m->info.src_endpoint,
                                       cluster_id,
                                       attr_id,
                                       (uint8_t)vtype,
                                       vbool,
                                       vi64,
                                       (float)vf64,
                                       NULL,
                                       NULL);
                }
            }
        }

        ESP_LOGD(TAG, "read attr response processed: uid=%s short=0x%04x", uid.uid, (unsigned)src_short);
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        const esp_zb_zcl_set_attr_value_message_t *m = (const esp_zb_zcl_set_attr_value_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }

        if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && m->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            uint8_t onoff = 0;
            if (m->attribute.data.value != NULL && m->attribute.data.size >= 1) {
                onoff = *((const uint8_t *)m->attribute.data.value);
            }

            ESP_LOGI(TAG, "onoff attr dst_ep=%u value=%u", (unsigned)m->info.dst_endpoint, (unsigned)onoff);
        }

        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID) {
        const esp_zb_zcl_privilege_command_message_t *m = (const esp_zb_zcl_privilege_command_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }
        const bool known_cluster =
            (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) ||
            (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL);
        if (!known_cluster) {
            return ESP_OK;
        }

        uint16_t src_short = 0;
        if (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
            src_short = m->info.src_address.u.short_addr;
        }

        gw_device_uid_t uid = {0};
        if (!gw_zb_model_find_uid_by_short(src_short, &uid) && src_short != 0) {
            (void)gw_zigbee_discover_by_short(src_short);
        }

        const char *cmd_name = zb_cmd_name(m->info.cluster, m->info.command.id);
        uart_send_zb_event(GW_PROTO_EVENT_COMMAND,
                           &uid,
                           src_short,
                           m->info.src_endpoint,
                           m->info.cluster,
                           0,
                           GW_PROTO_EVENT_VALUE_NONE,
                           false,
                           0,
                           0.0f,
                           cmd_name,
                           NULL);
    }

    return ESP_OK;
}

/* Note: Please select the correct console output port based on the development board in menuconfig */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
esp_err_t esp_zb_gateway_console_init(void)
{
    esp_err_t ret = ESP_OK;
    /* Disable buffering on stdin */
    setvbuf(stdin, NULL, _IONBF, 0);

    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    /* Move the caret to the beginning of the next line on '\n' */
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    /* Enable non-blocking mode on stdin and stdout */
    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ret = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    usb_serial_jtag_vfs_use_driver();
    uart_vfs_dev_register();
    return ret;
}
#endif

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee bdb commissioning");
}

static void bdb_close_network_cb(uint8_t unused)
{
    (void)unused;
    esp_err_t err = esp_zb_bdb_close_network();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Closing permit join after device authorization");
    } else {
        ESP_LOGW(TAG, "esp_zb_bdb_close_network() failed after device authorization: %s", esp_err_to_name(err));
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    esp_zb_zdo_signal_device_annce_params_t *dev_annce_params = NULL;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network formation");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ESP_LOGI(TAG, "Device rebooted (join closed by default, open via Web UI permit_join)");
                announce_snapshot_runtime_ready_once();
            }
        } else {
            ESP_LOGE(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ieee_address;
            esp_zb_get_long_address(ieee_address);
            ESP_LOGI(TAG, "Formed network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     ieee_address[7], ieee_address[6], ieee_address[5], ieee_address[4],
                     ieee_address[3], ieee_address[2], ieee_address[1], ieee_address[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            ESP_LOGI(TAG, "Network formed (join closed by default, open via Web UI permit_join)");
            announce_snapshot_runtime_ready_once();
        } else {
            ESP_LOGI(TAG, "Restart network formation (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Network steering started");
            announce_snapshot_runtime_ready_once();
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        dev_annce_params = (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "New device commissioned or rejoined (short: 0x%04hx)", dev_annce_params->device_short_addr);
        gw_zigbee_on_device_annce(dev_annce_params->ieee_addr, dev_annce_params->device_short_addr, dev_annce_params->capability);
        break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        esp_zb_zdo_signal_leave_indication_params_t *params =
            (esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        char uid[32] = {0};
        ieee_to_uid_string(params->device_addr, uid, sizeof(uid));
        ESP_LOGW(TAG,
                 "leave indication uid=%s short=0x%04hx rejoin=%u status=%s",
                 uid,
                 params->short_addr,
                 (unsigned)params->rejoin,
                 esp_err_to_name(err_status));
        break;
    }
    case ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        esp_zb_zdo_signal_device_update_params_t *params =
            (esp_zb_zdo_signal_device_update_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        char uid[32] = {0};
        ieee_to_uid_string(params->long_addr, uid, sizeof(uid));
        ESP_LOGW(TAG,
                 "device update uid=%s short=0x%04hx status=0x%02x tc_action=0x%02x parent=0x%04hx status_name=%s",
                 uid,
                 params->short_addr,
                 (unsigned)params->status,
                 (unsigned)params->tc_action,
                 params->parent_short,
                 esp_err_to_name(err_status));
        break;
    }
    case ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        esp_zb_zdo_signal_device_authorized_params_t *params =
            (esp_zb_zdo_signal_device_authorized_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        char uid[32] = {0};
        ieee_to_uid_string(params->long_addr, uid, sizeof(uid));
        ESP_LOGW(TAG,
                 "device authorized uid=%s short=0x%04hx auth_type=0x%02x auth_status=0x%02x",
                 uid,
                 params->short_addr,
                 (unsigned)params->authorization_type,
                 (unsigned)params->authorization_status);
        ESP_LOGI(TAG, "Device authorized; closing permit join");
        esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_close_network_cb, 0, 1);
        break;
    }
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK) {
            if (*(uint8_t *)esp_zb_app_signal_get_params(p_sg_p)) {
                ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", esp_zb_get_pan_id(), *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p));
            } else {
                ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", esp_zb_get_pan_id());
            }
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
        ESP_LOGI(TAG, "Production configuration is %s", err_status == ESP_OK ? "ready" : "not present");
        esp_zb_set_node_descriptor_manufacturer_code(ESP_MANUFACTURER_CODE);
        break;
    case ESP_ZB_NLME_STATUS_INDICATION:
    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        // Chatty status signals; skip to reduce log noise and CPU usage.
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = ESP_ZB_GATEWAY_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_REMOTE_CONTROL_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_zb_attribute_list_t *basic_cluser = esp_zb_basic_cluster_create(NULL);
    esp_zb_basic_cluster_add_attr(basic_cluser, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_cluser, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, ESP_MODEL_IDENTIFIER);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluser, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Groups server enables receiving group-addressed commands (and helps with interoperability).
    esp_zb_groups_cluster_cfg_t groups_cfg = {.groups_name_support_id = 0};
    esp_zb_cluster_list_add_groups_cluster(cluster_list, esp_zb_groups_cluster_create(&groups_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Expose an On/Off server so HA switches can bind to the gateway and we can observe toggle commands.
    esp_zb_on_off_cluster_cfg_t on_off_cfg = {.on_off = false};
    esp_zb_cluster_list_add_on_off_cluster(cluster_list, esp_zb_on_off_cluster_create(&on_off_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    // Expose a Level server to receive dimmer commands from remotes.
    esp_zb_level_cluster_cfg_t level_cfg = {.current_level = ESP_ZB_ZCL_LEVEL_CONTROL_CURRENT_LEVEL_DEFAULT_VALUE};
    esp_zb_cluster_list_add_level_cluster(cluster_list, esp_zb_level_cluster_create(&level_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_ep_list_add_gateway_ep(ep_list, cluster_list, endpoint_config);
    esp_zb_device_register(ep_list);

    // Allow the application to observe controller commands as "privilege command" callbacks.
    esp_zb_core_action_handler_register(zb_core_action_handler);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CMD_ON_OFF_ON_ID);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE_TO_LEVEL);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STEP);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STOP);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE_TO_LEVEL_WITH_ON_OFF);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_MOVE_WITH_ON_OFF);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STEP_WITH_ON_OFF);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CMD_LEVEL_CONTROL_STOP_WITH_ON_OFF);
    ESP_LOGI(TAG, "registered privilege handlers for onoff+level");

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Keep Zigbee runtime visible; UART transport stays quiet unless something is wrong.
    esp_log_level_set("gw_uart", ESP_LOG_WARN);
    esp_log_level_set("gw_zigbee", ESP_LOG_INFO);
    esp_log_level_set("ESP_ZB_GATEWAY", ESP_LOG_INFO);

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };

    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gw_zb_model_init());
    ESP_ERROR_CHECK(gw_deleted_devices_init());
    ESP_ERROR_CHECK(gw_device_registry_init());
    ESP_ERROR_CHECK(gw_uart_link_start());
    ESP_LOGI(TAG, "c6 thin zigbee router started");
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    ESP_ERROR_CHECK(esp_zb_gateway_console_init());
#endif
    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, GW_TASK_PRIO_ZIGBEE, NULL);
}



