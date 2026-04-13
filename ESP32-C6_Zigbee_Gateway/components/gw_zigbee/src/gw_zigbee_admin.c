#include "gw_zigbee/gw_zigbee.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "esp_zigbee_core.h"
#include "esp_zigbee_secur.h"
#include "test/esp_zigbee_test_utils.h"
#include "zdo/esp_zigbee_zdo_command.h"

#include "gw_core/c6_store.h"
#include "gw_zigbee_internal.h"

static const char *TAG = "gw_zigbee";

static bool uid_str_to_ieee(const char *uid, esp_zb_ieee_addr_t out_ieee)
{
    if (uid == NULL || out_ieee == NULL) {
        return false;
    }
    if (strncmp(uid, "0x", 2) != 0 && strncmp(uid, "0X", 2) != 0) {
        return false;
    }

    char *end = NULL;
    unsigned long long v = strtoull(uid + 2, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }

    for (int i = 7; i >= 0; i--) {
        out_ieee[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
    return true;
}

typedef struct {
    uint8_t token;
    gw_device_uid_t uid;
    uint16_t short_addr;
    bool rejoin;
    esp_zb_zdo_mgmt_leave_req_param_t req;
} gw_zb_leave_ctx_t;

static gw_zb_leave_ctx_t *s_leave_ctx_by_token[256];
static uint8_t s_leave_token;
static portMUX_TYPE s_leave_lock = portMUX_INITIALIZER_UNLOCKED;
static const uint32_t GW_LEAVE_TIMEOUT_MS = 12000;

static gw_zb_leave_ctx_t *leave_find_ctx_for_uid_locked(const gw_device_uid_t *uid, uint8_t *out_token)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(s_leave_ctx_by_token) / sizeof(s_leave_ctx_by_token[0]); ++i) {
        gw_zb_leave_ctx_t *ctx = s_leave_ctx_by_token[i];
        if (ctx != NULL && strncmp(ctx->uid.uid, uid->uid, sizeof(ctx->uid.uid)) == 0) {
            if (out_token != NULL) {
                *out_token = (uint8_t)i;
            }
            return ctx;
        }
    }
    return NULL;
}

static void leave_finalize_success(gw_zb_leave_ctx_t *ctx, const char *msg)
{
    if (!ctx) {
        return;
    }

    (void)gw_c6_store_device_remove(&ctx->uid);
    gw_zigbee_uart_send_event(GW_PROTO_EVENT_DEVICE_LEAVE,
                              ctx->uid.uid,
                              ctx->short_addr,
                              0,
                              0,
                              0,
                              GW_PROTO_EVENT_VALUE_TEXT,
                              false,
                              0,
                              0.0f,
                              NULL,
                              msg);
    gw_zigbee_request_snapshot_refresh();
}

static void purge_local_device_state(const gw_device_uid_t *uid)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return;
    }

    esp_err_t remove_err = gw_c6_store_device_set_status(uid, GW_DEVICE_STATUS_QUARANTINED);
    ESP_LOGW(TAG,
             "mark device quarantined uid=%s status=%s",
             uid->uid,
             esp_err_to_name(remove_err));
    gw_zigbee_request_snapshot_refresh();
}

static void leave_timeout_cb(uint8_t token)
{
    gw_zb_leave_ctx_t *ctx = NULL;

    portENTER_CRITICAL(&s_leave_lock);
    ctx = s_leave_ctx_by_token[token];
    s_leave_ctx_by_token[token] = NULL;
    portEXIT_CRITICAL(&s_leave_lock);

    if (ctx == NULL) {
        return;
    }

    (void)gw_c6_store_device_set_status(&ctx->uid, GW_DEVICE_STATUS_QUARANTINED);
    gw_zigbee_log_diag("leave_timeout", ctx->uid.uid, ctx->short_addr, "leave response timeout; device remains quarantined");
    free(ctx);
}

static void leave_resp_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    gw_zb_leave_ctx_t *ctx = (gw_zb_leave_ctx_t *)user_ctx;
    if (ctx == NULL) {
        return;
    }

    char msg[64];
    (void)snprintf(msg, sizeof(msg), "status=0x%02x rejoin=%u", (unsigned)zdo_status, ctx->rejoin ? 1U : 0U);
    gw_zigbee_log_diag((zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) ? "leave_ok" : "leave_failed",
                       ctx->uid.uid,
                       ctx->short_addr,
                       msg);
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        gw_zigbee_lock();
        esp_zb_scheduler_alarm_cancel(leave_timeout_cb, ctx->token);
        gw_zigbee_unlock();

        portENTER_CRITICAL(&s_leave_lock);
        if (s_leave_ctx_by_token[ctx->token] == ctx) {
            s_leave_ctx_by_token[ctx->token] = NULL;
        }
        portEXIT_CRITICAL(&s_leave_lock);
        gw_zigbee_log_diag("leave_removed", ctx->uid.uid, ctx->short_addr, "leave completed; device removed");
        leave_finalize_success(ctx, msg);
        free(ctx);
    }
}

static void leave_send_cb(uint8_t token)
{
    gw_zb_leave_ctx_t *ctx = NULL;

    portENTER_CRITICAL(&s_leave_lock);
    ctx = s_leave_ctx_by_token[token];
    portEXIT_CRITICAL(&s_leave_lock);

    if (ctx == NULL) {
        return;
    }

    esp_zb_zdo_device_leave_req(&ctx->req, leave_resp_cb, ctx);
}

esp_err_t gw_zigbee_device_leave(const gw_device_uid_t *uid, uint16_t short_addr, bool rejoin)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t existing_token = 0;
    gw_zb_leave_ctx_t *existing_ctx = NULL;
    portENTER_CRITICAL(&s_leave_lock);
    existing_ctx = leave_find_ctx_for_uid_locked(uid, &existing_token);
    if (existing_ctx != NULL) {
        if (short_addr != 0 && short_addr != 0xFFFF) {
            existing_ctx->short_addr = short_addr;
            existing_ctx->req.dst_nwk_addr = short_addr;
        }
        existing_ctx->rejoin = rejoin;
        existing_ctx->req.rejoin = rejoin ? 1 : 0;
        portEXIT_CRITICAL(&s_leave_lock);

        gw_zigbee_lock();
        esp_zb_scheduler_alarm_cancel(leave_timeout_cb, existing_token);
        esp_zb_scheduler_alarm(leave_send_cb, existing_token, 0);
        esp_zb_scheduler_alarm(leave_timeout_cb, existing_token, GW_LEAVE_TIMEOUT_MS);
        gw_zigbee_unlock();

        gw_zigbee_log_diag("leave_pending_updated", uid->uid, existing_ctx->short_addr, rejoin ? "rejoin=1" : "rejoin=0");
        return ESP_OK;
    }

    portEXIT_CRITICAL(&s_leave_lock);

    gw_zb_leave_ctx_t *ctx = (gw_zb_leave_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ctx->uid = *uid;
    ctx->short_addr = short_addr;
    ctx->rejoin = rejoin;

    if (!uid_str_to_ieee(uid->uid, ctx->req.device_address)) {
        free(ctx);
        return ESP_ERR_INVALID_ARG;
    }
    ctx->req.dst_nwk_addr = short_addr;
    ctx->req.remove_children = 0;
    ctx->req.rejoin = rejoin ? 1 : 0;
    uint8_t token = 0;
    portENTER_CRITICAL(&s_leave_lock);
    s_leave_token++;
    if (s_leave_token == 0) {
        s_leave_token++;
    }
    token = s_leave_token;
    if (s_leave_ctx_by_token[token] != NULL) {
        portEXIT_CRITICAL(&s_leave_lock);
        free(ctx);
        return ESP_FAIL;
    }
    ctx->token = token;
    s_leave_ctx_by_token[token] = ctx;
    portEXIT_CRITICAL(&s_leave_lock);

    (void)gw_c6_store_device_set_status(uid, GW_DEVICE_STATUS_LEAVE_REQUESTED);
    gw_zigbee_log_diag("leave_mark_requested", uid->uid, short_addr, "marked leave_requested");

    purge_local_device_state(uid);

    gw_zigbee_log_diag("leave_requested", uid->uid, short_addr, rejoin ? "rejoin=1" : "rejoin=0");

    gw_zigbee_lock();
    esp_zb_scheduler_alarm(leave_send_cb, token, 0);
    esp_zb_scheduler_alarm(leave_timeout_cb, token, GW_LEAVE_TIMEOUT_MS);
    gw_zigbee_unlock();
    return ESP_OK;
}

bool gw_zigbee_leave_observe_indication(const gw_device_uid_t *uid, uint16_t short_addr)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return false;
    }

    uint8_t token = 0;
    gw_zb_leave_ctx_t *ctx = NULL;
    portENTER_CRITICAL(&s_leave_lock);
    ctx = leave_find_ctx_for_uid_locked(uid, &token);
    if (ctx != NULL) {
        s_leave_ctx_by_token[token] = NULL;
    }
    portEXIT_CRITICAL(&s_leave_lock);

    if (ctx == NULL) {
        return false;
    }

    gw_zigbee_lock();
    esp_zb_scheduler_alarm_cancel(leave_timeout_cb, token);
    gw_zigbee_unlock();

    if (short_addr != 0 && short_addr != 0xFFFF) {
        ctx->short_addr = short_addr;
    }
    gw_zigbee_log_diag("leave_indication_observed", ctx->uid.uid, ctx->short_addr, "leave indication matched pending leave");
    leave_finalize_success(ctx, "leave_indication");
    free(ctx);
    return true;
}

static void permit_join_cb(uint8_t seconds)
{
    esp_err_t err = esp_zb_bdb_open_network(seconds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_zb_bdb_open_network(%u) failed: %s", (unsigned)seconds, esp_err_to_name(err));
        gw_zigbee_log_diag("permit_join_failed", "", 0, "esp_zb_bdb_open_network failed");
        return;
    }

    ESP_LOGW(TAG, "permit_join enabled for %u seconds", (unsigned)seconds);

    char msg[48];
    (void)snprintf(msg, sizeof(msg), "seconds=%u", (unsigned)seconds);
    gw_zigbee_log_diag("permit_join_enabled", "", 0, msg);
}

esp_err_t gw_zigbee_permit_join(uint8_t seconds)
{
    if (seconds == 0) {
        seconds = 180;
    }

    gw_zigbee_lock();
    esp_zb_scheduler_alarm(permit_join_cb, seconds, 0);
    gw_zigbee_unlock();
    return ESP_OK;
}

typedef struct {
    bool unbind;
    gw_device_uid_t src_uid;
    gw_device_uid_t dst_uid;
    uint16_t src_short;
    uint8_t src_ep;
    uint16_t cluster_id;
    uint8_t dst_ep;
    esp_zb_ieee_addr_t src_ieee;
    esp_zb_ieee_addr_t dst_ieee;
} gw_zb_bind_req_ctx_t;

static gw_zb_bind_req_ctx_t *s_bind_req_ctx_by_token[256];
static uint8_t s_bind_req_token;
static portMUX_TYPE s_bind_req_lock = portMUX_INITIALIZER_UNLOCKED;

static void bind_req_send_cb(uint8_t token)
{
    gw_zb_bind_req_ctx_t *ctx = NULL;
    portENTER_CRITICAL(&s_bind_req_lock);
    ctx = s_bind_req_ctx_by_token[token];
    s_bind_req_ctx_by_token[token] = NULL;
    portEXIT_CRITICAL(&s_bind_req_lock);

    if (ctx == NULL) {
        return;
    }

    char msg[160];
    (void)snprintf(msg,
                   sizeof(msg),
                   "%s cluster=0x%04x src_ep=%u -> dst=%s ep=%u",
                   ctx->unbind ? "unbind" : "bind",
                   (unsigned)ctx->cluster_id,
                   (unsigned)ctx->src_ep,
                   ctx->dst_uid.uid,
                   (unsigned)ctx->dst_ep);
    gw_zigbee_log_diag(ctx->unbind ? "unbind_requested" : "bind_requested", ctx->src_uid.uid, ctx->src_short, msg);

    gw_zb_bind_ctx_t *bctx = (gw_zb_bind_ctx_t *)calloc(1, sizeof(*bctx));
    if (bctx == NULL) {
        gw_zigbee_log_diag(ctx->unbind ? "unbind_failed" : "bind_failed", ctx->src_uid.uid, ctx->src_short, "no mem for bind ctx");
        free(ctx);
        return;
    }
    bctx->uid = ctx->src_uid;
    bctx->short_addr = ctx->src_short;
    bctx->src_ep = ctx->src_ep;
    bctx->cluster_id = ctx->cluster_id;
    bctx->dst_ep = ctx->dst_ep;
    bctx->unbind = ctx->unbind;
    strlcpy(bctx->dst_uid, ctx->dst_uid.uid, sizeof(bctx->dst_uid));

    esp_zb_zdo_bind_req_param_t bind = {0};
    memcpy(bind.src_address, ctx->src_ieee, sizeof(bind.src_address));
    bind.src_endp = ctx->src_ep;
    bind.cluster_id = ctx->cluster_id;
    bind.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
    memcpy(bind.dst_address_u.addr_long, ctx->dst_ieee, sizeof(bind.dst_address_u.addr_long));
    bind.dst_endp = ctx->dst_ep;
    bind.req_dst_addr = ctx->src_short;

    if (ctx->unbind) {
        esp_zb_zdo_device_unbind_req(&bind, gw_zigbee_bind_resp_cb, bctx);
    } else {
        esp_zb_zdo_device_bind_req(&bind, gw_zigbee_bind_resp_cb, bctx);
    }

    free(ctx);
}

static esp_err_t schedule_bind_req(const gw_zb_bind_req_ctx_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_zb_bind_req_ctx_t *ctx = (gw_zb_bind_req_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *ctx = *in;

    uint8_t token = 0;
    portENTER_CRITICAL(&s_bind_req_lock);
    s_bind_req_token++;
    if (s_bind_req_token == 0) {
        s_bind_req_token++;
    }
    token = s_bind_req_token;
    if (s_bind_req_ctx_by_token[token] != NULL) {
        portEXIT_CRITICAL(&s_bind_req_lock);
        free(ctx);
        return ESP_FAIL;
    }
    s_bind_req_ctx_by_token[token] = ctx;
    portEXIT_CRITICAL(&s_bind_req_lock);

    gw_zigbee_lock();
    esp_zb_scheduler_alarm(bind_req_send_cb, token, 0);
    gw_zigbee_unlock();
    return ESP_OK;
}

esp_err_t gw_zigbee_bind(const gw_device_uid_t *src_uid,
                         uint8_t src_endpoint,
                         uint16_t cluster_id,
                         const gw_device_uid_t *dst_uid,
                         uint8_t dst_endpoint)
{
    if (src_uid == NULL || dst_uid == NULL || src_uid->uid[0] == '\0' || dst_uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (src_endpoint == 0 || dst_endpoint == 0 || cluster_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_t src = {0};
    if (gw_c6_store_device_get(src_uid, &src) != ESP_OK || src.short_addr == 0 || src.short_addr == 0xFFFF) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_zb_bind_req_ctx_t ctx = {0};
    ctx.unbind = false;
    ctx.src_uid = *src_uid;
    ctx.dst_uid = *dst_uid;
    ctx.src_short = src.short_addr;
    ctx.src_ep = src_endpoint;
    ctx.cluster_id = cluster_id;
    ctx.dst_ep = dst_endpoint;
    if (!uid_str_to_ieee(src_uid->uid, ctx.src_ieee)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!uid_str_to_ieee(dst_uid->uid, ctx.dst_ieee)) {
        return ESP_ERR_INVALID_ARG;
    }

    return schedule_bind_req(&ctx);
}

esp_err_t gw_zigbee_unbind(const gw_device_uid_t *src_uid,
                           uint8_t src_endpoint,
                           uint16_t cluster_id,
                           const gw_device_uid_t *dst_uid,
                           uint8_t dst_endpoint)
{
    if (src_uid == NULL || dst_uid == NULL || src_uid->uid[0] == '\0' || dst_uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (src_endpoint == 0 || dst_endpoint == 0 || cluster_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_t src = {0};
    if (gw_c6_store_device_get(src_uid, &src) != ESP_OK || src.short_addr == 0 || src.short_addr == 0xFFFF) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_zb_bind_req_ctx_t ctx = {0};
    ctx.unbind = true;
    ctx.src_uid = *src_uid;
    ctx.dst_uid = *dst_uid;
    ctx.src_short = src.short_addr;
    ctx.src_ep = src_endpoint;
    ctx.cluster_id = cluster_id;
    ctx.dst_ep = dst_endpoint;
    if (!uid_str_to_ieee(src_uid->uid, ctx.src_ieee)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!uid_str_to_ieee(dst_uid->uid, ctx.dst_ieee)) {
        return ESP_ERR_INVALID_ARG;
    }

    return schedule_bind_req(&ctx);
}
