#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_proto/gw_proto.h"
#include "gw_proto/gw_proto_types.h"
#include "zdo/esp_zigbee_zdo_common.h"

// Keep in sync with main/esp_zigbee_gateway.h (ESP_ZB_GATEWAY_ENDPOINT).
#define GW_ZIGBEE_GATEWAY_ENDPOINT 1

extern const int16_t gw_zigbee_report_change_temp_01c;
extern const uint16_t gw_zigbee_report_change_hum_01pct;
extern const uint8_t gw_zigbee_report_change_batt_halfpct;
extern const uint8_t gw_zigbee_report_change_level;
extern const uint16_t gw_zigbee_report_change_color_xy;
extern const uint16_t gw_zigbee_report_change_color_temp;

typedef struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
    uint8_t src_ep;
    uint16_t cluster_id;
    uint8_t dst_ep;
    bool unbind;
    char dst_uid[GW_DEVICE_UID_STRLEN];
} gw_zb_bind_ctx_t;

void gw_zigbee_request_snapshot_refresh(void);
void gw_zigbee_bind_resp_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx);

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
                               const char *value_text);

void gw_zigbee_log_device_action(const char *stage,
                                 const char *uid,
                                 uint16_t short_addr,
                                 uint8_t endpoint,
                                 const char *cmd,
                                 const char *cluster,
                                 uint32_t arg0,
                                 uint32_t arg1);

void gw_zigbee_log_group_action(const char *stage,
                                uint16_t group_id,
                                const char *cmd,
                                const char *cluster,
                                uint32_t arg0,
                                uint32_t arg1);

void gw_zigbee_log_diag(const char *kind,
                        const char *device_uid,
                        uint16_t short_addr,
                        const char *msg);

void gw_zigbee_lock(void);
void gw_zigbee_unlock(void);
void gw_zigbee_ieee_to_uid_str(const uint8_t ieee_addr[8], char out[GW_DEVICE_UID_STRLEN]);
bool gw_zigbee_cluster_list_has(const uint16_t *list, uint8_t count, uint16_t cluster_id);

bool gw_zigbee_leave_observe_indication(const gw_device_uid_t *uid, uint16_t short_addr);
