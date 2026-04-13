#include "gw_zigbee/gw_zigbee_events.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"
#include "zdo/esp_zigbee_zdo_command.h"

#include "gw_core/c6_store.h"
#include "gw_zigbee/gw_zigbee.h"
#include "gw_zigbee_internal.h"

static const char *TAG = "gw_zb_events";

typedef struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
} gw_zb_quarantine_leave_ctx_t;

typedef struct {
    bool active;
    gw_device_uid_t uid;
    uint16_t short_addr;
    uint8_t expected_eps;
    uint8_t responded_eps;
    uint8_t endpoint_ids[GW_DEVICE_MAX_ENDPOINTS];
    bool endpoint_done[GW_DEVICE_MAX_ENDPOINTS];
} gw_zb_discovery_session_t;

static gw_zb_discovery_session_t s_sessions[GW_DEVICE_MAX_DEVICES];
static struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
    char stage[16];
    uint64_t ts_ms;
} s_last_discovery_fail;

// Runtime events that survive policy checks are forwarded to S3 unchanged.
static esp_err_t handle_forward_event(const gw_proto_event_v1_t *evt);

static gw_zb_discovery_session_t *find_session_by_uid(const gw_device_uid_t *uid)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < GW_DEVICE_MAX_DEVICES; ++i) {
        if (s_sessions[i].active && strcmp(s_sessions[i].uid.uid, uid->uid) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static gw_zb_discovery_session_t *find_session_by_short(uint16_t short_addr)
{
    if (short_addr == 0 || short_addr == 0xFFFF) {
        return NULL;
    }
    for (size_t i = 0; i < GW_DEVICE_MAX_DEVICES; ++i) {
        if (s_sessions[i].active && s_sessions[i].short_addr == short_addr) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static gw_zb_discovery_session_t *alloc_session(const gw_device_uid_t *uid, uint16_t short_addr)
{
    gw_zb_discovery_session_t *session = find_session_by_uid(uid);
    if (session != NULL) {
        memset(session, 0, sizeof(*session));
    } else {
        for (size_t i = 0; i < GW_DEVICE_MAX_DEVICES; ++i) {
            if (!s_sessions[i].active) {
                session = &s_sessions[i];
                memset(session, 0, sizeof(*session));
                break;
            }
        }
    }
    if (session == NULL) {
        return NULL;
    }
    session->active = true;
    session->uid = *uid;
    session->short_addr = short_addr;
    return session;
}

static void clear_session(gw_zb_discovery_session_t *session)
{
    if (session != NULL) {
        memset(session, 0, sizeof(*session));
    }
}

static bool device_has_real_endpoints(const gw_device_uid_t *uid)
{
    gw_device_full_t device = {0};
    if (gw_c6_store_device_get_full(uid, &device) != ESP_OK) {
        return false;
    }
    for (uint8_t i = 0; i < device.endpoint_count && i < GW_DEVICE_MAX_ENDPOINTS; ++i) {
        const gw_device_endpoint_t *ep = &device.endpoints[i];
        if (ep->profile_id != 0 || ep->device_id != 0 || ep->in_cluster_count != 0 || ep->out_cluster_count != 0) {
            return true;
        }
    }
    return false;
}

static void finalize_discovery_session(gw_zb_discovery_session_t *session)
{
    if (session == NULL) {
        return;
    }

    gw_device_t d = {0};
    if (gw_c6_store_device_get(&session->uid, &d) == ESP_OK) {
        d.short_addr = session->short_addr;
        d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
        d.status = device_has_real_endpoints(&session->uid) ? GW_DEVICE_STATUS_READY : GW_DEVICE_STATUS_NEW;
        (void)gw_c6_store_device_upsert(&d);
        (void)gw_c6_store_device_sync_endpoints(&d.device_uid);
    }

    ESP_LOGW(TAG,
             "discovery complete: %s short=0x%04x endpoints=%u/%u status=%s",
             session->uid.uid,
             (unsigned)session->short_addr,
             (unsigned)session->responded_eps,
             (unsigned)session->expected_eps,
             device_has_real_endpoints(&session->uid) ? "ready" : "new");
    clear_session(session);
    gw_zigbee_request_snapshot_refresh();
}

static void note_simple_desc_result(const gw_device_uid_t *uid, uint16_t short_addr, uint8_t endpoint)
{
    gw_zb_discovery_session_t *session = find_session_by_uid(uid);
    if (session == NULL || !session->active) {
        return;
    }
    if (session->expected_eps == 0) {
        return;
    }

    for (uint8_t i = 0; i < session->expected_eps && i < GW_DEVICE_MAX_ENDPOINTS; ++i) {
        if (session->endpoint_ids[i] != endpoint) {
            continue;
        }
        if (!session->endpoint_done[i]) {
            session->endpoint_done[i] = true;
            session->responded_eps++;
        }
        break;
    }

    if (session->responded_eps >= session->expected_eps) {
        finalize_discovery_session(session);
    } else {
        ESP_LOGW(TAG,
                 "discovery progress: %s short=0x%04x ep=%u %u/%u",
                 uid->uid,
                 (unsigned)short_addr,
                 (unsigned)endpoint,
                 (unsigned)session->responded_eps,
                 (unsigned)session->expected_eps);
    }
}

static void request_bind_to_gateway(const char *uid,
                                    const uint8_t src_ieee[8],
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

static bool uid_to_ieee_addr(const gw_device_uid_t *uid, uint8_t out_ieee[8])
{
    if (uid == NULL || out_ieee == NULL) {
        return false;
    }
    if (strncmp(uid->uid, "0x", 2) != 0 && strncmp(uid->uid, "0X", 2) != 0) {
        return false;
    }

    char *end = NULL;
    unsigned long long v = strtoull(uid->uid + 2, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }

    for (int i = 7; i >= 0; --i) {
        out_ieee[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
    return true;
}

static void ieee_to_uid(const uint8_t ieee_addr[8], gw_device_uid_t *out_uid)
{
    if (ieee_addr == NULL || out_uid == NULL) {
        return;
    }
    memset(out_uid, 0, sizeof(*out_uid));
    gw_zigbee_ieee_to_uid_str(ieee_addr, out_uid->uid);
}

static void close_network_cb(uint8_t unused)
{
    (void)unused;
    esp_err_t err = esp_zb_bdb_close_network();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Closing permit join after device authorization");
    } else {
        ESP_LOGW(TAG, "esp_zb_bdb_close_network() failed after device authorization: %s", esp_err_to_name(err));
    }
}

static void quarantine_leave_task(void *arg)
{
    gw_zb_quarantine_leave_ctx_t *ctx = (gw_zb_quarantine_leave_ctx_t *)arg;
    if (ctx != NULL) {
        vTaskDelay(pdMS_TO_TICKS(250));
        (void)gw_zigbee_device_leave(&ctx->uid, ctx->short_addr, false);
        free(ctx);
    }
    vTaskDelete(NULL);
}

static void schedule_quarantine_leave(const gw_device_uid_t *uid, uint16_t short_addr)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return;
    }

    gw_zb_quarantine_leave_ctx_t *ctx = (gw_zb_quarantine_leave_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        gw_zigbee_log_diag("quarantine_schedule_failed", uid->uid, short_addr, "no mem for quarantine leave ctx");
        return;
    }

    ctx->uid = *uid;
    ctx->short_addr = short_addr;

    BaseType_t ok = xTaskCreate(quarantine_leave_task, "zb_q_leave", 3072, ctx, 5, NULL);
    if (ok != pdPASS) {
        gw_zigbee_log_diag("quarantine_schedule_failed", uid->uid, short_addr, "xTaskCreate failed");
        free(ctx);
    }
}

// Quarantine and leave-indication paths use the same canonical local purge.
static void purge_local_device_model(const gw_device_uid_t *uid, uint16_t short_addr, const char *reason)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return;
    }

    esp_err_t err = gw_c6_store_device_remove(uid);
    ESP_LOGW(TAG,
             "purge local device uid=%s short=0x%04x reason=%s full_remove=%s",
             uid->uid,
             (unsigned)short_addr,
             (reason != NULL) ? reason : "",
             esp_err_to_name(err));
    gw_zigbee_request_snapshot_refresh();
}

static void upsert_runtime_device(const gw_device_uid_t *uid,
                                  uint16_t short_addr,
                                  gw_device_status_t status)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return;
    }

    gw_device_t device = {0};
    if (gw_c6_store_device_get(uid, &device) != ESP_OK) {
        device.device_uid = *uid;
    }

    if (short_addr != 0 && short_addr != 0xFFFF) {
        device.short_addr = short_addr;
    }
    device.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (status != GW_DEVICE_STATUS_NONE) {
        device.status = status;
    }
    (void)gw_c6_store_device_upsert(&device);
}

static bool device_status_is_quarantined(gw_device_status_t status)
{
    return status == GW_DEVICE_STATUS_QUARANTINED || status == GW_DEVICE_STATUS_LEAVE_REQUESTED;
}

static bool recently_failed_active_ep(const gw_device_uid_t *uid, uint16_t short_addr)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return false;
    }
    if (strcmp(s_last_discovery_fail.uid.uid, uid->uid) != 0) {
        return false;
    }
    if (s_last_discovery_fail.short_addr != short_addr) {
        return false;
    }
    if (strcmp(s_last_discovery_fail.stage, "active_ep") != 0) {
        return false;
    }
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    return (now_ms >= s_last_discovery_fail.ts_ms) && ((now_ms - s_last_discovery_fail.ts_ms) <= 15000);
}

static bool is_device_quarantined(const gw_device_uid_t *uid)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return false;
    }
    gw_device_t device = {0};
    if (gw_c6_store_device_get(uid, &device) != ESP_OK) {
        return false;
    }
    return device_status_is_quarantined(device.status);
}

static void start_join_discovery(const gw_device_uid_t *uid,
                                 uint16_t short_addr,
                                 const char *reason)
{
    if (uid == NULL || uid->uid[0] == '\0' || short_addr == 0 || short_addr == 0xFFFF) {
        return;
    }

    gw_device_full_t existing = {0};
    if (gw_c6_store_device_get_full(uid, &existing) == ESP_OK) {
        if (existing.status == GW_DEVICE_STATUS_DISCOVERING && find_session_by_uid(uid) != NULL) {
            char msg[96];
            (void)snprintf(msg, sizeof(msg), "discover_by_short suppressed 0x%04x (%s, already discovering)",
                           (unsigned)short_addr, reason ? reason : "join");
            gw_zigbee_log_diag("join_discovery_suppressed", uid->uid, short_addr, msg);
            return;
        }
        if (existing.status == GW_DEVICE_STATUS_READY && device_has_real_endpoints(uid)) {
            return;
        }
    }

    upsert_runtime_device(uid, short_addr, GW_DEVICE_STATUS_DISCOVERING);
    if (alloc_session(uid, short_addr) == NULL) {
        gw_zigbee_log_diag("join_discovery_failed", uid->uid, short_addr, "no mem for discovery session");
        return;
    }

    esp_err_t err = gw_zigbee_discover_by_short(short_addr);
    if (err == ESP_OK) {
        char msg[80];
        (void)snprintf(msg, sizeof(msg), "discover_by_short 0x%04x (%s)", (unsigned)short_addr, reason ? reason : "join");
        gw_zigbee_log_diag("join_discovery", uid->uid, short_addr, msg);
    } else if (err == ESP_ERR_INVALID_STATE) {
        const bool allow_recovery =
            (reason != NULL) &&
            (strcmp(reason, "device_authorized") == 0) &&
            recently_failed_active_ep(uid, short_addr);
        if (allow_recovery) {
            esp_err_t forced = gw_zigbee_discover_by_short_force(short_addr);
            if (forced == ESP_OK) {
                char msg[112];
                (void)snprintf(msg,
                               sizeof(msg),
                               "discover_by_short forced 0x%04x (%s, recovery after active_ep fail)",
                               (unsigned)short_addr,
                               reason);
                gw_zigbee_log_diag("join_discovery_recovery", uid->uid, short_addr, msg);
                return;
            }
        }

        clear_session(find_session_by_uid(uid));
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "discover_by_short suppressed 0x%04x (%s, throttled)",
                       (unsigned)short_addr, reason ? reason : "join");
        gw_zigbee_log_diag("join_discovery_suppressed", uid->uid, short_addr, msg);
    } else {
        clear_session(find_session_by_uid(uid));
        char msg[80];
        (void)snprintf(msg, sizeof(msg), "discover_by_short failed: %s", esp_err_to_name(err));
        gw_zigbee_log_diag("join_discovery_failed", uid->uid, short_addr, msg);
    }
}

bool gw_zigbee_handle_quarantine_hit(const gw_device_uid_t *uid, uint16_t short_addr, const char *reason)
{
    if (uid == NULL || uid->uid[0] == '\0') {
        return false;
    }
    if (!is_device_quarantined(uid)) {
        return false;
    }

    gw_zigbee_log_diag("quarantine_hit", uid->uid, short_addr, (reason != NULL) ? reason : "quarantined device");
    schedule_quarantine_leave(uid, short_addr);
    return true;
}

static bool event_is_quarantined(const gw_proto_event_v1_t *evt)
{
    if (evt == NULL || evt->device_uid.uid[0] == '\0') {
        return false;
    }
    return is_device_quarantined(&evt->device_uid);
}

static void log_device_event(const char *label, const gw_proto_event_v1_t *evt)
{
    if (evt == NULL) {
        return;
    }

    ESP_LOGW(TAG,
             "%s uid=%s short=0x%04x status=0x%04x aux=0x%04x parent=0x%04x flags=0x%02x",
             label,
             evt->device_uid.uid,
             (unsigned)evt->short_addr,
             (unsigned)evt->status_code,
             (unsigned)evt->aux_u16,
             (unsigned)evt->parent_short_addr,
             (unsigned)evt->flags);
}

static esp_err_t handle_device_annce(const gw_proto_event_v1_t *evt)
{
    uint8_t ieee[8] = {0};
    if (!uid_to_ieee_addr(&evt->device_uid, ieee)) {
        ESP_LOGW(TAG, "device annce ignored: bad uid=%s", evt->device_uid.uid);
        return ESP_ERR_INVALID_ARG;
    }
    gw_zigbee_on_device_annce(ieee, evt->short_addr, (uint8_t)(evt->aux_u16 & 0xFFu));
    return ESP_OK;
}

static esp_err_t handle_leave_indication(const gw_proto_event_v1_t *evt)
{
    log_device_event("leave indication", evt);
    if (gw_zigbee_leave_observe_indication(&evt->device_uid, evt->short_addr)) {
        return ESP_OK;
    }
    purge_local_device_model(&evt->device_uid, evt->short_addr, "leave_indication");
    return ESP_OK;
}

static esp_err_t handle_device_update(const gw_proto_event_v1_t *evt)
{
    log_device_event("device update", evt);
    if (event_is_quarantined(evt)) {
        (void)gw_zigbee_handle_quarantine_hit(&evt->device_uid,
                                              evt->short_addr,
                                              "device update for quarantined device, forcing leave");
        return ESP_OK;
    }

    start_join_discovery(&evt->device_uid, evt->short_addr, "device_update");
    return ESP_OK;
}

static esp_err_t handle_device_authorized(const gw_proto_event_v1_t *evt)
{
    log_device_event("device authorized", evt);
    if (event_is_quarantined(evt)) {
        (void)gw_zigbee_handle_quarantine_hit(&evt->device_uid,
                                              evt->short_addr,
                                              "device authorized while quarantined, forcing leave");
        return ESP_OK;
    }

    start_join_discovery(&evt->device_uid, evt->short_addr, "device_authorized");

    ESP_LOGW(TAG, "Device authorized; closing permit join");
    esp_zb_scheduler_alarm((esp_zb_callback_t)close_network_cb, 0, 1);
    return ESP_OK;
}

esp_err_t gw_zigbee_handle_device_announced(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability)
{
    if (ieee_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_t d = {0};
    ieee_to_uid(ieee_addr, &d.device_uid);

    if (gw_zigbee_handle_quarantine_hit(&d.device_uid, short_addr, "device re-announced, scheduling leave")) {
        ESP_LOGW(TAG,
                 "Quarantined device re-announced: %s short=0x%04x cap=0x%02x",
                 d.device_uid.uid,
                 (unsigned)short_addr,
                 (unsigned)capability);
        return ESP_OK;
    }

    gw_device_t existing = {0};
    if (gw_c6_store_device_get(&d.device_uid, &existing) == ESP_OK) {
        if ((existing.status == GW_DEVICE_STATUS_DISCOVERING && find_session_by_uid(&d.device_uid) != NULL) ||
            (existing.status == GW_DEVICE_STATUS_READY && device_has_real_endpoints(&d.device_uid))) {
            ESP_LOGW(TAG,
                     "device announce ignored: %s short=0x%04x status=%u",
                     d.device_uid.uid,
                     (unsigned)short_addr,
                     (unsigned)existing.status);
            return ESP_ERR_INVALID_STATE;
        }
        d = existing;
    }

    d.short_addr = short_addr;
    d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    d.status = GW_DEVICE_STATUS_DISCOVERING;

    esp_err_t err = gw_c6_store_device_upsert(&d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registry upsert failed for %s: %s", d.device_uid.uid, esp_err_to_name(err));
        gw_zigbee_log_diag("join_failed", d.device_uid.uid, d.short_addr, "device registry upsert failed");
        return err;
    }

    ESP_LOGW(TAG, "Device announced: %s short=0x%04x cap=0x%02x", d.device_uid.uid, (unsigned)d.short_addr, (unsigned)capability);
    (void)gw_c6_store_device_sync_endpoints(&d.device_uid);

    char msg[64];
    (void)snprintf(msg, sizeof(msg), "cap=0x%02x", (unsigned)capability);

    gw_proto_event_v1_t evt = {0};
    evt.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    evt.event_id_kind = GW_PROTO_EVENT_DEVICE_JOIN;
    evt.device_uid = d.device_uid;
    evt.short_addr = d.short_addr;
    evt.value_type = GW_PROTO_EVENT_VALUE_TEXT;
    strlcpy(evt.value_text, msg, sizeof(evt.value_text));
    return handle_forward_event(&evt);
}

esp_err_t gw_zigbee_handle_ieee_resolved(const uint8_t ieee_addr[8], uint16_t short_addr)
{
    if (ieee_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_uid_t duid = {0};
    ieee_to_uid(ieee_addr, &duid);

    if (gw_zigbee_handle_quarantine_hit(&duid, short_addr, "device resolved by short lookup, scheduling leave")) {
        ESP_LOGW(TAG, "Quarantined device resolved by short lookup: %s short=0x%04x", duid.uid, (unsigned)short_addr);
        return ESP_OK;
    }

    gw_device_t d = {0};
    d.device_uid = duid;
    d.short_addr = short_addr;
    d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    d.status = GW_DEVICE_STATUS_DISCOVERING;
    return gw_c6_store_device_upsert(&d);
}

bool gw_zigbee_handle_active_ep_discovered(const uint8_t ieee_addr[8],
                                           uint16_t short_addr,
                                           const uint8_t *ep_ids,
                                           uint8_t ep_count)
{
    if (ieee_addr == NULL) {
        return false;
    }

    gw_device_uid_t duid = {0};
    ieee_to_uid(ieee_addr, &duid);
    if (gw_zigbee_handle_quarantine_hit(&duid, short_addr, "active ep for quarantined device, scheduling leave")) {
        ESP_LOGW(TAG, "Quarantined device active ep ignored: %s short=0x%04x", duid.uid, (unsigned)short_addr);
        return false;
    }

    upsert_runtime_device(&duid, short_addr, GW_DEVICE_STATUS_DISCOVERING);

    gw_zb_discovery_session_t *session = find_session_by_uid(&duid);
    if (session == NULL) {
        session = alloc_session(&duid, short_addr);
    }
    if (session == NULL) {
        gw_zigbee_log_diag("join_discovery_failed", duid.uid, short_addr, "no mem for discovery session");
        return false;
    }
    if (session->expected_eps > 0) {
        ESP_LOGW(TAG,
                 "active ep ignored: %s short=0x%04x discovery already in progress %u/%u",
                 duid.uid,
                 (unsigned)short_addr,
                 (unsigned)session->responded_eps,
                 (unsigned)session->expected_eps);
        return false;
    }
    session->short_addr = short_addr;
    session->expected_eps = (ep_count > GW_DEVICE_MAX_ENDPOINTS) ? GW_DEVICE_MAX_ENDPOINTS : ep_count;
    session->responded_eps = 0;
    for (uint8_t i = 0; i < session->expected_eps; ++i) {
        session->endpoint_ids[i] = ep_ids[i];
        session->endpoint_done[i] = false;
    }

    char msg[64];
    (void)snprintf(msg, sizeof(msg), "ep_count=%u", (unsigned)ep_count);
    ESP_LOGW(TAG, "active ep: %s short=0x%04x %s", duid.uid, (unsigned)short_addr, msg);
    return true;
}

esp_err_t gw_zigbee_handle_simple_desc_discovered(const uint8_t ieee_addr[8],
                                                  uint16_t short_addr,
                                                  const gw_zb_endpoint_t *ep)
{
    if (ieee_addr == NULL || ep == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_uid_t duid = ep->uid;
    if (gw_zigbee_handle_quarantine_hit(&duid, short_addr, "simple desc for quarantined device, scheduling leave")) {
        ESP_LOGW(TAG,
                 "Quarantined device simple desc ignored: %s short=0x%04x ep=%u",
                 duid.uid,
                 (unsigned)short_addr,
                 (unsigned)ep->endpoint);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gw_c6_store_endpoint_upsert(ep);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "endpoint upsert failed for %s ep=%u: %s", duid.uid, (unsigned)ep->endpoint, esp_err_to_name(err));
        return err;
    }

    char msg[160];
    (void)snprintf(msg,
                   sizeof(msg),
                   "ep=%u profile=0x%04x dev=0x%04x in=%u out=%u kind=%s",
                   (unsigned)ep->endpoint,
                   (unsigned)ep->profile_id,
                   (unsigned)ep->device_id,
                   (unsigned)ep->in_cluster_count,
                   (unsigned)ep->out_cluster_count,
                   ep->kind);
    ESP_LOGW(TAG, "simple desc: %s short=0x%04x %s", duid.uid, (unsigned)short_addr, msg);

    gw_device_t d = {0};
    if (gw_c6_store_device_get(&duid, &d) == ESP_OK) {
        d.short_addr = short_addr;
        d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
        d.status = GW_DEVICE_STATUS_DISCOVERING;
        (void)gw_c6_store_device_upsert(&d);
    } else {
        d.device_uid = duid;
        d.short_addr = short_addr;
        d.last_seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
        d.status = GW_DEVICE_STATUS_DISCOVERING;
        (void)gw_c6_store_device_upsert(&d);
    }

    note_simple_desc_result(&duid, short_addr, ep->endpoint);

    return ESP_OK;
}

void gw_zigbee_handle_simple_desc_failed(const uint8_t ieee_addr[8],
                                         uint16_t short_addr,
                                         uint8_t endpoint,
                                         uint8_t zdo_status)
{
    if (ieee_addr == NULL) {
        return;
    }

    gw_device_uid_t uid = {0};
    ieee_to_uid(ieee_addr, &uid);
    ESP_LOGW(TAG,
             "simple desc failed tracked: %s short=0x%04x ep=%u status=0x%02x",
             uid.uid,
             (unsigned)short_addr,
             (unsigned)endpoint,
             (unsigned)zdo_status);
    note_simple_desc_result(&uid, short_addr, endpoint);
}

void gw_zigbee_handle_discovery_failed(uint16_t short_addr, const char *stage)
{
    gw_zb_discovery_session_t *session = find_session_by_short(short_addr);
    if (session == NULL) {
        return;
    }

    ESP_LOGW(TAG,
             "discovery failed: %s short=0x%04x stage=%s",
             session->uid.uid,
             (unsigned)short_addr,
             stage != NULL ? stage : "");
    s_last_discovery_fail.uid = session->uid;
    s_last_discovery_fail.short_addr = short_addr;
    strlcpy(s_last_discovery_fail.stage, stage != NULL ? stage : "", sizeof(s_last_discovery_fail.stage));
    s_last_discovery_fail.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    (void)gw_c6_store_device_set_status(&session->uid, GW_DEVICE_STATUS_NEW);
    clear_session(session);
}

void gw_zigbee_handle_simple_desc_bindings(const uint8_t ieee_addr[8],
                                           uint16_t short_addr,
                                           const gw_zb_endpoint_t *ep)
{
    if (ieee_addr == NULL || ep == NULL) {
        return;
    }

    const char *uid = ep->uid.uid;
    const bool has_onoff_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    const bool has_temp_meas_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
    const bool has_hum_meas_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
    const bool has_power_cfg_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    const bool has_level_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL);
    const bool has_color_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL);
    const bool has_occ_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, 0x0406);
    const bool has_illum_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, 0x0400);
    const bool has_pressure_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, 0x0403);
    const bool has_onoff_cli = gw_zigbee_cluster_list_has(ep->out_clusters, ep->out_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    const bool has_tuya_private_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, 0xE000);
    const bool has_tuya_private_cli = gw_zigbee_cluster_list_has(ep->out_clusters, ep->out_cluster_count, 0xE000);

    if (has_temp_meas_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_hum_meas_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_power_cfg_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_onoff_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_level_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_color_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_occ_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, 0x0406, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_illum_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, 0x0400, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_pressure_srv) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, 0x0403, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_onoff_cli) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, GW_ZIGBEE_GATEWAY_ENDPOINT);
    if (has_tuya_private_srv || has_tuya_private_cli) request_bind_to_gateway(uid, ieee_addr, short_addr, ep->endpoint, 0xE000, GW_ZIGBEE_GATEWAY_ENDPOINT);
}

void gw_zigbee_handle_simple_desc_reporting(uint16_t short_addr, const gw_zb_endpoint_t *ep)
{
    if (ep == NULL) {
        return;
    }

    const char *uid = ep->uid.uid;
    char msg[96];
    const bool has_temp_meas_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
    const bool has_hum_meas_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
    const bool has_power_cfg_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    const bool has_onoff_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    const bool has_level_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL);
    const bool has_color_srv = gw_zigbee_cluster_list_has(ep->in_clusters, ep->in_cluster_count, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL);

    if (has_temp_meas_srv) {
        esp_zb_zcl_config_report_record_t rec = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_S16, .min_interval = 5, .max_interval = 60, .reportable_change = (void *)&gw_zigbee_report_change_temp_01c};
        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; cmd.zcl_basic_cmd.dst_endpoint = ep->endpoint; cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT; cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; cmd.record_number = 1; cmd.record_field = &rec;
        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report temp ep=%u tsn=%u", (unsigned)ep->endpoint, (unsigned)tsn); gw_zigbee_log_diag("config_report", uid, short_addr, msg);
        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID}; esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; r.zcl_basic_cmd.dst_endpoint = ep->endpoint; r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT; r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; r.attr_number = 1; r.attr_field = attrs; (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }
    if (has_hum_meas_srv) {
        esp_zb_zcl_config_report_record_t rec = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_U16, .min_interval = 5, .max_interval = 60, .reportable_change = (void *)&gw_zigbee_report_change_hum_01pct};
        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; cmd.zcl_basic_cmd.dst_endpoint = ep->endpoint; cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT; cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; cmd.record_number = 1; cmd.record_field = &rec;
        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report humidity ep=%u tsn=%u", (unsigned)ep->endpoint, (unsigned)tsn); gw_zigbee_log_diag("config_report", uid, short_addr, msg);
        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID}; esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; r.zcl_basic_cmd.dst_endpoint = ep->endpoint; r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT; r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; r.attr_number = 1; r.attr_field = attrs; (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }
    if (has_power_cfg_srv) {
        esp_zb_zcl_config_report_record_t rec = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_U8, .min_interval = 300, .max_interval = 3600, .reportable_change = (void *)&gw_zigbee_report_change_batt_halfpct};
        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; cmd.zcl_basic_cmd.dst_endpoint = ep->endpoint; cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG; cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; cmd.record_number = 1; cmd.record_field = &rec;
        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report battery ep=%u tsn=%u", (unsigned)ep->endpoint, (unsigned)tsn); gw_zigbee_log_diag("config_report", uid, short_addr, msg);
        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID}; esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; r.zcl_basic_cmd.dst_endpoint = ep->endpoint; r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG; r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; r.attr_number = 1; r.attr_field = attrs; (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }
    if (has_onoff_srv) {
        esp_zb_zcl_config_report_record_t rec = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_BOOL, .min_interval = 0, .max_interval = 300, .reportable_change = NULL};
        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; cmd.zcl_basic_cmd.dst_endpoint = ep->endpoint; cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF; cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; cmd.record_number = 1; cmd.record_field = &rec;
        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report onoff ep=%u tsn=%u", (unsigned)ep->endpoint, (unsigned)tsn); gw_zigbee_log_diag("config_report", uid, short_addr, msg);
        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID}; esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; r.zcl_basic_cmd.dst_endpoint = ep->endpoint; r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF; r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; r.attr_number = 1; r.attr_field = attrs; (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }
    if (has_level_srv) {
        esp_zb_zcl_config_report_record_t rec = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_U8, .min_interval = 1, .max_interval = 60, .reportable_change = (void *)&gw_zigbee_report_change_level};
        esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; cmd.zcl_basic_cmd.dst_endpoint = ep->endpoint; cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL; cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; cmd.record_number = 1; cmd.record_field = &rec;
        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report level ep=%u tsn=%u", (unsigned)ep->endpoint, (unsigned)tsn); gw_zigbee_log_diag("config_report", uid, short_addr, msg);
        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID}; esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; r.zcl_basic_cmd.dst_endpoint = ep->endpoint; r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL; r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; r.attr_number = 1; r.attr_field = attrs; (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }
    if (has_color_srv) {
        esp_zb_zcl_config_report_record_t rec_xy_x = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_U16, .min_interval = 1, .max_interval = 60, .reportable_change = (void *)&gw_zigbee_report_change_color_xy};
        esp_zb_zcl_config_report_record_t rec_xy_y = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_U16, .min_interval = 1, .max_interval = 60, .reportable_change = (void *)&gw_zigbee_report_change_color_xy};
        esp_zb_zcl_config_report_record_t rec_ct = {.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND, .attributeID = ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, .attrType = ESP_ZB_ZCL_ATTR_TYPE_U16, .min_interval = 1, .max_interval = 60, .reportable_change = (void *)&gw_zigbee_report_change_color_temp};
        esp_zb_zcl_config_report_record_t recs[] = {rec_xy_x, rec_xy_y, rec_ct}; esp_zb_zcl_config_report_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; cmd.zcl_basic_cmd.dst_endpoint = ep->endpoint; cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL; cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; cmd.record_number = (uint8_t)(sizeof(recs) / sizeof(recs[0])); cmd.record_field = recs;
        uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&cmd);
        (void)snprintf(msg, sizeof(msg), "config_report color ep=%u tsn=%u", (unsigned)ep->endpoint, (unsigned)tsn); gw_zigbee_log_diag("config_report", uid, short_addr, msg);
        uint16_t attrs[] = {ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID}; esp_zb_zcl_read_attr_cmd_t r = {0};
        r.zcl_basic_cmd.dst_addr_u.addr_short = short_addr; r.zcl_basic_cmd.dst_endpoint = ep->endpoint; r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT; r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; r.clusterID = ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL; r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV; r.attr_number = 3; r.attr_field = attrs; (void)esp_zb_zcl_read_attr_cmd_req(&r);
    }
}

static esp_err_t handle_forward_event(const gw_proto_event_v1_t *evt)
{
    if (evt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (event_is_quarantined(evt)) {
        gw_zigbee_log_diag("quarantine_hit",
                           evt->device_uid.uid,
                           evt->short_addr,
                           "runtime event for quarantined device ignored");
        schedule_quarantine_leave(&evt->device_uid, evt->short_addr);
        return ESP_OK;
    }

    gw_zigbee_uart_send_event(evt->event_id_kind,
                              evt->device_uid.uid,
                              evt->short_addr,
                              evt->endpoint,
                              evt->cluster_id,
                              evt->attr_id,
                              evt->value_type,
                              evt->value_bool != 0,
                              evt->value_i64,
                              evt->value_f32,
                              evt->cmd[0] ? evt->cmd : NULL,
                              evt->value_text[0] ? evt->value_text : NULL);
    return ESP_OK;
}

esp_err_t gw_zigbee_handle_event(const gw_proto_event_v1_t *evt)
{
    if (evt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (evt->event_id_kind) {
    case GW_PROTO_EVENT_DEVICE_ANNCE:
        return handle_device_annce(evt);
    case GW_PROTO_EVENT_LEAVE_INDICATION:
        return handle_leave_indication(evt);
    case GW_PROTO_EVENT_DEVICE_UPDATE:
        return handle_device_update(evt);
    case GW_PROTO_EVENT_DEVICE_AUTHORIZED:
        return handle_device_authorized(evt);
    case GW_PROTO_EVENT_ATTR_REPORT:
    case GW_PROTO_EVENT_COMMAND:
    case GW_PROTO_EVENT_DEVICE_JOIN:
    case GW_PROTO_EVENT_DEVICE_LEAVE:
    case GW_PROTO_EVENT_NET_STATE:
        return handle_forward_event(evt);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
