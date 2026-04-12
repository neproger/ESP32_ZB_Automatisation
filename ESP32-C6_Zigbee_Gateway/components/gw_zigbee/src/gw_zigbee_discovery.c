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

#include "gw_zigbee/gw_zigbee_events.h"
#include "gw_zigbee_internal.h"

static const char *TAG = "gw_zigbee";

static void format_cluster_list(const uint16_t *clusters, uint8_t count, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!clusters || count == 0) {
        strlcpy(out, "-", out_len);
        return;
    }

    size_t used = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const int written = snprintf(out + used,
                                     (used < out_len) ? (out_len - used) : 0,
                                     "%s0x%04x",
                                     (i == 0) ? "" : ",",
                                     (unsigned)clusters[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= out_len - used) {
            used = out_len - 1;
            break;
        }
        used += (size_t)written;
    }
}

static void format_endpoint_list(const uint8_t *eps, uint8_t count, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!eps || count == 0) {
        strlcpy(out, "-", out_len);
        return;
    }

    size_t used = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const int written = snprintf(out + used,
                                     (used < out_len) ? (out_len - used) : 0,
                                     "%s%u",
                                     (i == 0) ? "" : ",",
                                     (unsigned)eps[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= out_len - used) {
            used = out_len - 1;
            break;
        }
        used += (size_t)written;
    }
}

typedef struct {
    esp_zb_ieee_addr_t ieee;
    uint16_t short_addr;
    uint8_t active_ep_retry_count;
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
static gw_zb_discover_ctx_t *s_pending_active_ep_retry;
static const uint32_t GW_ACTIVE_EP_RETRY_DELAY_MS = 1200;

static void active_ep_retry_cb(uint8_t token);

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    gw_zb_simple_ctx_t *ctx = (gw_zb_simple_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || simple_desc == NULL || simple_desc->app_cluster_list == NULL) {
        ESP_LOGW(TAG, "simple desc failed: short=0x%04x status=0x%02x", (unsigned)ctx->short_addr, (unsigned)zdo_status);
        gw_zigbee_handle_simple_desc_failed(ctx->ieee, ctx->short_addr, ctx->endpoint, (uint8_t)zdo_status);
        free(ctx);
        return;
    }

    const uint16_t *in_clusters = &simple_desc->app_cluster_list[0];
    const uint16_t *out_clusters = &simple_desc->app_cluster_list[simple_desc->app_input_cluster_count];

    char in_buf[128];
    char out_buf[128];
    format_cluster_list(in_clusters, simple_desc->app_input_cluster_count, in_buf, sizeof(in_buf));
    format_cluster_list(out_clusters, simple_desc->app_output_cluster_count, out_buf, sizeof(out_buf));
    ESP_LOGI(TAG,
             "simple desc raw: short=0x%04x ep=%u profile=0x%04x dev=0x%04x in[%u]=%s out[%u]=%s",
             (unsigned)ctx->short_addr,
             (unsigned)simple_desc->endpoint,
             (unsigned)simple_desc->app_profile_id,
             (unsigned)simple_desc->app_device_id,
             (unsigned)simple_desc->app_input_cluster_count,
             in_buf,
             (unsigned)simple_desc->app_output_cluster_count,
             out_buf);

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
    strlcpy(ep.kind, "zigbee device", sizeof(ep.kind));
    if (gw_zigbee_handle_simple_desc_discovered(ctx->ieee, ctx->short_addr, &ep) != ESP_OK) {
        free(ctx);
        return;
    }

    gw_zigbee_handle_simple_desc_bindings(ctx->ieee, ctx->short_addr, &ep);
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
        if (ctx->active_ep_retry_count < 1) {
            ctx->active_ep_retry_count++;
            if (s_pending_active_ep_retry != NULL) {
                free(ctx);
                return;
            }
            s_pending_active_ep_retry = ctx;
            ESP_LOGW(TAG,
                     "active ep failed: short=0x%04x status=0x%02x; scheduling retry %u/%u",
                     (unsigned)ctx->short_addr,
                     (unsigned)zdo_status,
                     (unsigned)ctx->active_ep_retry_count,
                     1u);
            gw_zigbee_lock();
            esp_zb_scheduler_alarm(active_ep_retry_cb, 0, GW_ACTIVE_EP_RETRY_DELAY_MS);
            gw_zigbee_unlock();
            return;
        }

        ESP_LOGW(TAG, "active ep failed: short=0x%04x status=0x%02x", (unsigned)ctx->short_addr, (unsigned)zdo_status);
        gw_zigbee_handle_discovery_failed(ctx->short_addr, "active_ep");
        free(ctx);
        return;
    }

    char uid[GW_DEVICE_UID_STRLEN];
    gw_zigbee_ieee_to_uid_str(ctx->ieee, uid);

    char ep_buf[64];
    format_endpoint_list(ep_id_list, ep_count, ep_buf, sizeof(ep_buf));
    ESP_LOGI(TAG,
             "active ep raw: uid=%s short=0x%04x ep_count=%u eps=%s",
             uid,
             (unsigned)ctx->short_addr,
             (unsigned)ep_count,
             ep_buf);

    if (!gw_zigbee_handle_active_ep_discovered(ctx->ieee, ctx->short_addr, ep_id_list, ep_count)) {
        free(ctx);
        return;
    }

    for (uint8_t i = 0; i < ep_count; i++) {
        gw_zb_simple_ctx_t *sctx = (gw_zb_simple_ctx_t *)calloc(1, sizeof(*sctx));
        if (sctx == NULL) {
            ESP_LOGW(TAG, "simple desc ctx alloc failed: %s short=0x%04x", uid, (unsigned)ctx->short_addr);
            gw_zigbee_handle_simple_desc_failed(ctx->ieee, ctx->short_addr, ep_id_list[i], 0xFF);
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

static void active_ep_retry_cb(uint8_t token)
{
    (void)token;
    gw_zb_discover_ctx_t *ctx = s_pending_active_ep_retry;
    s_pending_active_ep_retry = NULL;
    if (ctx == NULL) {
        return;
    }
    esp_zb_zdo_active_ep_req_param_t req = {.addr_of_interest = ctx->short_addr};
    esp_zb_zdo_active_ep_req(&req, active_ep_cb, ctx);
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
    char uid[GW_DEVICE_UID_STRLEN];
    gw_zigbee_ieee_to_uid_str(ieee_addr, uid);
    ESP_LOGI(TAG, "discovery start: uid=%s short=0x%04x", uid, (unsigned)short_addr);
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
        gw_zigbee_handle_discovery_failed(ctx->short_addr, "ieee_lookup");
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

static esp_err_t gw_zigbee_discover_by_short_impl(uint16_t short_addr, bool force)
{
    if (short_addr == 0 || short_addr == 0xFFFF) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!force && should_throttle_discovery(short_addr)) {
        return ESP_ERR_INVALID_STATE;
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

esp_err_t gw_zigbee_discover_by_short(uint16_t short_addr)
{
    return gw_zigbee_discover_by_short_impl(short_addr, false);
}

esp_err_t gw_zigbee_discover_by_short_force(uint16_t short_addr)
{
    return gw_zigbee_discover_by_short_impl(short_addr, true);
}

void gw_zigbee_on_device_annce(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability)
{
    if (gw_zigbee_handle_device_announced(ieee_addr, short_addr, capability) == ESP_OK) {
        gw_zigbee_start_discovery(ieee_addr, short_addr);
    }
}

