#include "gw_zigbee_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "esp_zigbee_core.h"

#include "gw_core/gw_proto_uplink.h"

static const char *TAG = "gw_zigbee";

const int16_t gw_zigbee_report_change_temp_01c = 10;
const uint16_t gw_zigbee_report_change_hum_01pct = 100;
const uint8_t gw_zigbee_report_change_batt_halfpct = 2;
const uint8_t gw_zigbee_report_change_level = 1;
const uint16_t gw_zigbee_report_change_color_xy = 16;
const uint16_t gw_zigbee_report_change_color_temp = 10;

void gw_zigbee_request_snapshot_refresh(void)
{
    (void)gw_uart_link_request_snapshot_debounced();
}

void gw_zigbee_bind_resp_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    gw_zb_bind_ctx_t *ctx = (gw_zb_bind_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    char msg[64];
    (void)snprintf(msg,
                   sizeof(msg),
                   "status=0x%02x %s cluster=0x%04x src_ep=%u dst_ep=%u",
                   (unsigned)zdo_status,
                   ctx->unbind ? "unbind" : "bind",
                   (unsigned)ctx->cluster_id,
                   (unsigned)ctx->src_ep,
                   (unsigned)ctx->dst_ep);
    gw_zigbee_log_diag((zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) ? (ctx->unbind ? "unbind_ok" : "bind_ok")
                                                                 : (ctx->unbind ? "unbind_failed" : "bind_failed"),
                       ctx->uid.uid,
                       ctx->short_addr,
                       msg);
    free(ctx);
}

void gw_zigbee_uart_send_event(uint8_t event_kind,
                               const char *device_uid,
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
    if (device_uid) {
        strlcpy(evt.device_uid.uid, device_uid, sizeof(evt.device_uid.uid));
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

void gw_zigbee_log_device_action(const char *stage,
                                 const char *uid,
                                 uint16_t short_addr,
                                 uint8_t endpoint,
                                 const char *cmd,
                                 const char *cluster,
                                 uint32_t arg0,
                                 uint32_t arg1)
{
    ESP_LOGW(TAG,
             "%s uid=%s short=0x%04x ep=%u cmd=%s cluster=%s arg0=%u arg1=%u",
             stage ? stage : "action",
             uid ? uid : "",
             (unsigned)short_addr,
             (unsigned)endpoint,
             cmd ? cmd : "",
             cluster ? cluster : "",
             (unsigned)arg0,
             (unsigned)arg1);
}

void gw_zigbee_log_group_action(const char *stage,
                                uint16_t group_id,
                                const char *cmd,
                                const char *cluster,
                                uint32_t arg0,
                                uint32_t arg1)
{
    ESP_LOGW(TAG,
             "%s group=0x%04x cmd=%s cluster=%s arg0=%u arg1=%u",
             stage ? stage : "action",
             (unsigned)group_id,
             cmd ? cmd : "",
             cluster ? cluster : "",
             (unsigned)arg0,
             (unsigned)arg1);
}

void gw_zigbee_log_diag(const char *kind,
                        const char *device_uid,
                        uint16_t short_addr,
                        const char *msg)
{
    ESP_LOGW(TAG,
             "diag=%s uid=%s short=0x%04x %s",
             kind ? kind : "event",
             device_uid ? device_uid : "",
             (unsigned)short_addr,
             msg ? msg : "");
}

void gw_zigbee_lock(void)
{
    esp_zb_lock_acquire(portMAX_DELAY);
}

void gw_zigbee_unlock(void)
{
    esp_zb_lock_release();
}

void gw_zigbee_ieee_to_uid_str(const uint8_t ieee_addr[8], char out[GW_DEVICE_UID_STRLEN])
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)ieee_addr[i];
    }
    (void)snprintf(out, GW_DEVICE_UID_STRLEN, "0x%016" PRIx64, v);
}

bool gw_zigbee_cluster_list_has(const uint16_t *list, uint8_t count, uint16_t cluster_id)
{
    if (list == NULL || count == 0) {
        return false;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (list[i] == cluster_id) {
            return true;
        }
    }
    return false;
}
