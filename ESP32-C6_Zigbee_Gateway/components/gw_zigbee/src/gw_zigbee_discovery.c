#include "gw_zigbee/gw_zigbee.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "zdo/esp_zigbee_zdo_common.h"

#include "gw_core/device_registry.h"
#include "gw_core/zb_classify.h"
#include "gw_core/zb_model.h"
#include "gw_zigbee_internal.h"

static const char *TAG = "gw_zigbee";

typedef struct {
    esp_zb_ieee_addr_t ieee;
    uint16_t short_addr;
} gw_zb_discover_ctx_t;

typedef struct {
    esp_zb_ieee_addr_t ieee;
    uint16_t short_addr;
    uint8_t endpoint;
} gw_zb_simple_ctx_t;

typedef struct {
    uint16_t short_addr;
    esp_zb_zdo_ieee_addr_req_param_t req;
} gw_zb_ieee_lookup_ctx_t;

static gw_zb_ieee_lookup_ctx_t *s_ieee_ctx_by_token[256];
static uint8_t s_ieee_token;
static portMUX_TYPE s_ieee_lock = portMUX_INITIALIZER_UNLOCKED;

static void request_bind_to_gateway(const char *uid,
                                    const esp_zb_ieee_addr_t src_ieee,
                                    uint16_t short_addr,
                                    uint8_t src_ep,
                                    uint16_t cluster_id,
                                    uint8_t dst_ep)
{
    esp_zb_ieee_addr_t gw_ieee = {0};
    esp_zb_get_long_address(gw_ieee);

    gw_zb_bind_ctx_t *bctx = (gw_zb_bind_ctx_t *)calloc(1, sizeof(*bctx));
    if (bctx == NULL) {
        gw_zigbee_log_diag("bind_failed", uid, short_addr, "no mem for bind ctx");
        return;
    }

    strlcpy(bctx->uid.uid, uid, sizeof(bctx->uid.uid));
    bctx->short_addr = short_addr;
    bctx->src_ep = src_ep;
    bctx->cluster_id = cluster_id;
    bctx->dst_ep = dst_ep;
    bctx->unbind = false;
    bctx->dst_uid[0] = '\0';

    esp_zb_zdo_bind_req_param_t bind = {0};
    memcpy(bind.src_address, src_ieee, sizeof(bind.src_address));
    bind.src_endp = src_ep;
    bind.cluster_id = cluster_id;
    bind.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
    memcpy(bind.dst_address_u.addr_long, gw_ieee, sizeof(gw_ieee));
    bind.dst_endp = dst_ep;
    bind.req_dst_addr = short_addr;

    char msg[96];
    (void)snprintf(msg,
                   sizeof(msg),
                   "bind cluster=0x%04x src_ep=%u -> gw_ep=%u",
                   (unsigned)cluster_id,
                   (unsigned)src_ep,
                   (unsigned)dst_ep);
    gw_zigbee_log_diag("bind_requested", uid, short_addr, msg);
    esp_zb_zdo_device_bind_req(&bind, gw_zigbee_bind_resp_cb, bctx);
}

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    gw_zb_simple_ctx_t *ctx = (gw_zb_simple_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || simple_desc == NULL || simple_desc->app_cluster_list == NULL) {
        ESP_LOGW(TAG, "simple desc failed: short=0x%04x status=0x%02x", (unsigned)ctx->short_addr, (unsigned)zdo_status);
        gw_zigbee_request_snapshot_refresh();
        free(ctx);
        return;
    }

    const uint16_t *in_clusters = &simple_desc->app_cluster_list[0];
    const uint16_t *out_clusters = &simple_desc->app_cluster_list[simple_desc->app_input_cluster_count];

    bool has_groups_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_GROUPS);
    bool has_onoff_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    bool has_onoff_cli = gw_zigbee_cluster_list_has(out_clusters, simple_desc->app_output_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);

    const bool is_switch = has_onoff_cli;
    const bool is_light = (!is_switch && has_onoff_srv);

    char uid[GW_DEVICE_UID_STRLEN];
    gw_zigbee_ieee_to_uid_str(ctx->ieee, uid);

    gw_zb_endpoint_t ep = {0};
    strlcpy(ep.uid.uid, uid, sizeof(ep.uid.uid));
    ep.short_addr = ctx->short_addr;
    ep.endpoint = simple_desc->endpoint;
    ep.profile_id = simple_desc->app_profile_id;
    ep.device_id = simple_desc->app_device_id;
    ep.in_cluster_count =
        (simple_desc->app_input_cluster_count > GW_ZB_MAX_CLUSTERS) ? GW_ZB_MAX_CLUSTERS : simple_desc->app_input_cluster_count;
    ep.out_cluster_count =
        (simple_desc->app_output_cluster_count > GW_ZB_MAX_CLUSTERS) ? GW_ZB_MAX_CLUSTERS : simple_desc->app_output_cluster_count;
    memcpy(ep.in_clusters, in_clusters, ep.in_cluster_count * sizeof(ep.in_clusters[0]));
    memcpy(ep.out_clusters, out_clusters, ep.out_cluster_count * sizeof(ep.out_clusters[0]));
    const char *kind = gw_zb_endpoint_kind(&ep);

    (void)gw_zb_model_upsert_endpoint(&ep);

    char msg[160];
    (void)snprintf(msg,
                   sizeof(msg),
                   "ep=%u profile=0x%04x dev=0x%04x in=%u out=%u kind=%s groups=%d onoff_srv=%d onoff_cli=%d",
                   (unsigned)simple_desc->endpoint,
                   (unsigned)simple_desc->app_profile_id,
                   (unsigned)simple_desc->app_device_id,
                   (unsigned)simple_desc->app_input_cluster_count,
                   (unsigned)simple_desc->app_output_cluster_count,
                   kind,
                   has_groups_srv ? 1 : 0,
                   has_onoff_srv ? 1 : 0,
                   has_onoff_cli ? 1 : 0);
    ESP_LOGI(TAG, "simple desc: %s short=0x%04x %s", uid, (unsigned)ctx->short_addr, msg);
    gw_zigbee_request_snapshot_refresh();

    gw_device_uid_t duid = {0};
    strlcpy(duid.uid, uid, sizeof(duid.uid));

    gw_device_t d = {0};
    if (gw_device_registry_get(&duid, &d) == ESP_OK) {
        d.short_addr = ctx->short_addr;
        d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (is_switch) {
            d.has_button = true;
        }
        if (is_light) {
            d.has_onoff = true;
        }
        (void)gw_device_registry_upsert(&d);
        (void)gw_device_registry_sync_endpoints(&d.device_uid);
    }

    if (has_groups_srv && (is_switch || is_light)) {
        const uint16_t group_id = is_switch ? GW_ZIGBEE_GROUP_SWITCHES : GW_ZIGBEE_GROUP_LIGHTS;

        esp_zb_zcl_groups_add_group_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.group_id = group_id;

        uint8_t tsn = esp_zb_zcl_groups_add_group_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "add_group 0x%04x ep=%u tsn=%u", (unsigned)group_id, (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_zigbee_log_diag("group_add", uid, ctx->short_addr, msg);
    }

    const bool has_temp_meas_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
    const bool has_hum_meas_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
    const bool has_power_cfg_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    const bool has_level_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL);
    const bool has_color_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL);
    const bool has_occ_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, 0x0406);
    const bool has_illum_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, 0x0400);
    const bool has_pressure_srv = gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, 0x0403);

    if (has_temp_meas_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_hum_meas_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_power_cfg_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_onoff_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_level_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_color_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_occ_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, 0x0406, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_illum_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, 0x0400, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }
    if (has_pressure_srv) {
        request_bind_to_gateway(uid, ctx->ieee, ctx->short_addr, simple_desc->endpoint, 0x0403, GW_ZIGBEE_GATEWAY_ENDPOINT);
    }

    if (has_temp_meas_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_S16;
        rec.min_interval = 5;
        rec.max_interval = 60;
        rec.reportable_change = (void *)&gw_zigbee_report_change_temp_01c;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report temp ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_zigbee_log_diag("config_report", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (has_hum_meas_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_U16;
        rec.min_interval = 5;
        rec.max_interval = 60;
        rec.reportable_change = (void *)&gw_zigbee_report_change_hum_01pct;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report humidity ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_zigbee_log_diag("config_report", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (has_power_cfg_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_U8;
        rec.min_interval = 300;
        rec.max_interval = 3600;
        rec.reportable_change = (void *)&gw_zigbee_report_change_batt_halfpct;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report battery ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_zigbee_log_diag("config_report", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (has_onoff_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_BOOL;
        rec.min_interval = 0;
        rec.max_interval = 300;
        rec.reportable_change = NULL;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report onoff ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_zigbee_log_diag("config_report", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (has_level_srv) {
        esp_zb_zcl_config_report_record_t rec = {0};
        rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec.attributeID = ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID;
        rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_U8;
        rec.min_interval = 1;
        rec.max_interval = 60;
        rec.reportable_change = (void *)&gw_zigbee_report_change_level;

        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &rec;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report level ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_zigbee_log_diag("config_report", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID};
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 1;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (has_color_srv) {
        esp_zb_zcl_config_report_record_t rec_xy_x = {0};
        rec_xy_x.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec_xy_x.attributeID = ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID;
        rec_xy_x.attrType = ESP_ZB_ZCL_ATTR_TYPE_U16;
        rec_xy_x.min_interval = 1;
        rec_xy_x.max_interval = 60;
        rec_xy_x.reportable_change = (void *)&gw_zigbee_report_change_color_xy;

        esp_zb_zcl_config_report_record_t rec_xy_y = {0};
        rec_xy_y.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec_xy_y.attributeID = ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID;
        rec_xy_y.attrType = ESP_ZB_ZCL_ATTR_TYPE_U16;
        rec_xy_y.min_interval = 1;
        rec_xy_y.max_interval = 60;
        rec_xy_y.reportable_change = (void *)&gw_zigbee_report_change_color_xy;

        esp_zb_zcl_config_report_record_t rec_ct = {0};
        rec_ct.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        rec_ct.attributeID = ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID;
        rec_ct.attrType = ESP_ZB_ZCL_ATTR_TYPE_U16;
        rec_ct.min_interval = 1;
        rec_ct.max_interval = 60;
        rec_ct.reportable_change = (void *)&gw_zigbee_report_change_color_temp;

        esp_zb_zcl_config_report_record_t recs[] = {rec_xy_x, rec_xy_y, rec_ct};
        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = (uint8_t)(sizeof(recs) / sizeof(recs[0]));
        cmd.record_field = recs;

        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report color ep=%u tsn=%u", (unsigned)simple_desc->endpoint, (unsigned)tsn);
        gw_zigbee_log_diag("config_report", uid, ctx->short_addr, msg);

        uint16_t attrs[] = {
            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID,
            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID,
            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
        };
        esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        r.zcl_basic_cmd.dst_endpoint = simple_desc->endpoint;
        r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL;
        r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        r.attr_number = 3;
        r.attr_field = attrs;
        (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }

    if (is_switch) {
        esp_zb_ieee_addr_t gw_ieee = {0};
        esp_zb_get_long_address(gw_ieee);

        gw_zb_bind_ctx_t *bctx = (gw_zb_bind_ctx_t *)calloc(1, sizeof(*bctx));
        if (bctx != NULL) {
            strlcpy(bctx->uid.uid, uid, sizeof(bctx->uid.uid));
            bctx->short_addr = ctx->short_addr;
            bctx->src_ep = simple_desc->endpoint;
            bctx->cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
            bctx->dst_ep = GW_ZIGBEE_GATEWAY_ENDPOINT;
            bctx->unbind = false;
            bctx->dst_uid[0] = '\0';

            esp_zb_zdo_bind_req_param_t bind = {0};
            memcpy(bind.src_address, ctx->ieee, sizeof(bind.src_address));
            bind.src_endp = simple_desc->endpoint;
            bind.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
            bind.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
            memcpy(bind.dst_address_u.addr_long, gw_ieee, sizeof(gw_ieee));
            bind.dst_endp = GW_ZIGBEE_GATEWAY_ENDPOINT;
            bind.req_dst_addr = ctx->short_addr;

            (void)snprintf(msg,
                           sizeof(msg),
                           "bind on_off src_ep=%u -> gw_ep=%u",
                           (unsigned)bind.src_endp,
                           (unsigned)bind.dst_endp);
            gw_zigbee_log_diag("bind_requested", uid, ctx->short_addr, msg);

            esp_zb_zdo_device_bind_req(&bind, gw_zigbee_bind_resp_cb, bctx);
        } else {
            gw_zigbee_log_diag("bind_failed", uid, ctx->short_addr, "no mem for bind ctx");
        }
    }

    free(ctx);
}

static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_id_list, void *user_ctx)
{
    gw_zb_discover_ctx_t *ctx = (gw_zb_discover_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || ep_count == 0 || ep_id_list == NULL) {
        ESP_LOGW(TAG, "active ep failed: short=0x%04x status=0x%02x ep_count=%u", (unsigned)ctx->short_addr, (unsigned)zdo_status, (unsigned)ep_count);
        gw_zigbee_request_snapshot_refresh();
        free(ctx);
        return;
    }

    char uid[GW_DEVICE_UID_STRLEN];
    gw_zigbee_ieee_to_uid_str(ctx->ieee, uid);

    char msg[64];
    (void)snprintf(msg, sizeof(msg), "ep_count=%u", (unsigned)ep_count);
    ESP_LOGI(TAG, "active ep: %s short=0x%04x %s", uid, (unsigned)ctx->short_addr, msg);
    gw_zigbee_request_snapshot_refresh();

    for (uint8_t i = 0; i < ep_count; i++) {
        gw_zb_simple_ctx_t *sctx = (gw_zb_simple_ctx_t *)calloc(1, sizeof(*sctx));
        if (sctx == NULL) {
            ESP_LOGW(TAG, "simple desc ctx alloc failed: %s short=0x%04x", uid, (unsigned)ctx->short_addr);
            gw_zigbee_request_snapshot_refresh();
            continue;
        }
        memcpy(sctx->ieee, ctx->ieee, sizeof(sctx->ieee));
        sctx->short_addr = ctx->short_addr;
        sctx->endpoint = ep_id_list[i];

        esp_zb_zdo_simple_desc_req_param_t req = {
            .addr_of_interest = ctx->short_addr,
            .endpoint = sctx->endpoint,
        };
        esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, sctx);
    }

    free(ctx);
}

static void gw_zigbee_start_discovery(const uint8_t ieee_addr[8], uint16_t short_addr)
{
    gw_zb_discover_ctx_t *ctx = (gw_zb_discover_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        gw_zigbee_log_diag("discovery_failed", "", short_addr, "no mem for discovery ctx");
        return;
    }
    memcpy(ctx->ieee, ieee_addr, sizeof(ctx->ieee));
    ctx->short_addr = short_addr;
    esp_zb_zdo_active_ep_req_param_t req = {.addr_of_interest = short_addr};
    esp_zb_zdo_active_ep_req(&req, active_ep_cb, ctx);
}

static bool should_throttle_discovery(uint16_t short_addr)
{
    typedef struct {
        uint16_t short_addr;
        uint64_t ts_ms;
    } slot_t;

    static slot_t s_slots[8];
    static size_t s_next;

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    for (size_t i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        if (s_slots[i].short_addr == short_addr) {
            if (now_ms - s_slots[i].ts_ms < 30 * 1000) {
                return true;
            }
            s_slots[i].ts_ms = now_ms;
            return false;
        }
    }

    s_slots[s_next].short_addr = short_addr;
    s_slots[s_next].ts_ms = now_ms;
    s_next = (s_next + 1) % (sizeof(s_slots) / sizeof(s_slots[0]));
    return false;
}

static void ieee_addr_cb(esp_zb_zdp_status_t zdo_status, esp_zb_zdo_ieee_addr_rsp_t *resp, void *user_ctx)
{
    gw_zb_ieee_lookup_ctx_t *ctx = (gw_zb_ieee_lookup_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || resp == NULL) {
        gw_zigbee_log_diag("ieee_lookup_failed", "", ctx->short_addr, "ieee_addr_req failed");
        free(ctx);
        return;
    }

    char uid[GW_DEVICE_UID_STRLEN];
    gw_zigbee_ieee_to_uid_str(resp->ieee_addr, uid);

    gw_device_t d = {0};
    strlcpy(d.device_uid.uid, uid, sizeof(d.device_uid.uid));
    d.short_addr = resp->nwk_addr;
    d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    (void)gw_device_registry_upsert(&d);

    gw_zigbee_log_diag("ieee_lookup_ok", uid, resp->nwk_addr, "ieee resolved, starting discovery");
    gw_zigbee_start_discovery(resp->ieee_addr, resp->nwk_addr);

    free(ctx);
}

static void ieee_lookup_send_cb(uint8_t token)
{
    gw_zb_ieee_lookup_ctx_t *ctx = NULL;

    portENTER_CRITICAL(&s_ieee_lock);
    ctx = s_ieee_ctx_by_token[token];
    s_ieee_ctx_by_token[token] = NULL;
    portEXIT_CRITICAL(&s_ieee_lock);

    if (ctx == NULL) {
        return;
    }

    esp_zb_zdo_ieee_addr_req(&ctx->req, ieee_addr_cb, ctx);
}

esp_err_t gw_zigbee_discover_by_short(uint16_t short_addr)
{
    if (short_addr == 0 || short_addr == 0xFFFF) {
        return ESP_ERR_INVALID_ARG;
    }

    if (should_throttle_discovery(short_addr)) {
        ESP_LOGI(TAG, "discover_by_short throttled: short=0x%04x", (unsigned)short_addr);
        return ESP_OK;
    }

    gw_zb_ieee_lookup_ctx_t *ctx = (gw_zb_ieee_lookup_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->short_addr = short_addr;
    ctx->req.dst_nwk_addr = short_addr;
    ctx->req.addr_of_interest = short_addr;
    ctx->req.request_type = 0;
    ctx->req.start_index = 0;

    uint8_t token = 0;
    portENTER_CRITICAL(&s_ieee_lock);
    s_ieee_token++;
    if (s_ieee_token == 0) {
        s_ieee_token++;
    }
    token = s_ieee_token;
    if (s_ieee_ctx_by_token[token] != NULL) {
        portEXIT_CRITICAL(&s_ieee_lock);
        free(ctx);
        return ESP_FAIL;
    }
    s_ieee_ctx_by_token[token] = ctx;
    portEXIT_CRITICAL(&s_ieee_lock);

    gw_zigbee_log_diag("ieee_lookup_requested", "", short_addr, "ieee_addr_req");
    gw_zigbee_lock();
    esp_zb_scheduler_alarm(ieee_lookup_send_cb, token, 0);
    gw_zigbee_unlock();
    return ESP_OK;
}

void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability)
{
    if (ieee_addr == NULL) {
        return;
    }

    gw_device_t d = {0};
    gw_zigbee_ieee_to_uid_str(ieee_addr, d.device_uid.uid);

    {
        gw_device_t existing = {0};
        if (gw_device_registry_get(&d.device_uid, &existing) == ESP_OK) {
            d = existing;
        }
    }

    d.short_addr = short_addr;
    d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);

    esp_err_t err = gw_device_registry_upsert(&d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registry upsert failed for %s: %s", d.device_uid.uid, esp_err_to_name(err));
        gw_zigbee_log_diag("join_failed", d.device_uid.uid, d.short_addr, "device registry upsert failed");
        return;
    }

    ESP_LOGI(TAG, "Device announced: %s short=0x%04x cap=0x%02x", d.device_uid.uid, (unsigned)d.short_addr, (unsigned)capability);
    (void)gw_device_registry_sync_endpoints(&d.device_uid);
    {
        char msg[64];
        (void)snprintf(msg, sizeof(msg), "cap=0x%02x", (unsigned)capability);
        gw_zigbee_uart_send_event(GW_PROTO_EVENT_DEVICE_JOIN,
                                  d.device_uid.uid,
                                  d.short_addr,
                                  0,
                                  0,
                                  0,
                                  GW_PROTO_EVENT_VALUE_TEXT,
                                  false,
                                  0,
                                  0.0f,
                                  NULL,
                                  msg);
    }
    gw_zigbee_request_snapshot_refresh();

    gw_zigbee_start_discovery(ieee_addr, short_addr);
}

