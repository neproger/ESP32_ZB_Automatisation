#include "gw_zigbee/gw_zigbee.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "zdo/esp_zigbee_zdo_common.h"

#include "gw_core/zb_classify.h"
#include "gw_zigbee/gw_zigbee_events.h"
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

    const bool has_onoff_srv =
        gw_zigbee_cluster_list_has(in_clusters, simple_desc->app_input_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    const bool has_onoff_cli =
        gw_zigbee_cluster_list_has(out_clusters, simple_desc->app_output_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);

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
    if (gw_zigbee_handle_simple_desc_discovered(ctx->ieee, ctx->short_addr, &ep, is_switch, is_light, kind) != ESP_OK) {
        free(ctx);
        return;
    }

    gw_zigbee_handle_simple_desc_bindings(ctx->ieee, ctx->short_addr, &ep, is_switch, is_light);
    gw_zigbee_handle_simple_desc_reporting(ctx->short_addr, &ep);

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

    if (!gw_zigbee_handle_active_ep_discovered(ctx->ieee, ctx->short_addr, ep_count)) {
        free(ctx);
        return;
    }

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

// Keep noisy short->IEEE probes from stampeding the discovery pipeline.
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

    (void)gw_zigbee_handle_ieee_resolved(resp->ieee_addr, resp->nwk_addr);

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
    if (gw_zigbee_handle_device_announced(ieee_addr, short_addr, capability) == ESP_OK) {
        gw_zigbee_start_discovery(ieee_addr, short_addr);
    }
}

