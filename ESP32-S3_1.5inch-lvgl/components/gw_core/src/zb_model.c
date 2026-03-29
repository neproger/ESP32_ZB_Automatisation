#include "gw_core/zb_model.h"
#include "gw_core/device_registry.h"

#include <stdbool.h>
#include <string.h>

static bool s_inited;
static gw_zb_endpoint_t s_eps[GW_ZB_MAX_ENDPOINTS];
static size_t s_ep_count;
static uint32_t s_version_seq;

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a->uid, b->uid, sizeof(a->uid)) == 0;
}

esp_err_t gw_zb_model_init(void)
{
    s_inited = true;
    s_ep_count = 0;
    s_version_seq = 0;
    memset(s_eps, 0, sizeof(s_eps));
    return ESP_OK;
}

esp_err_t gw_zb_model_upsert_endpoint(const gw_zb_endpoint_t *ep)
{
    if (!s_inited || ep == NULL || ep->uid.uid[0] == '\0' || ep->endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_ep_count; i++) {
        if (uid_equals(&s_eps[i].uid, &ep->uid) && s_eps[i].endpoint == ep->endpoint) {
            const bool changed =
                (s_eps[i].short_addr != ep->short_addr) ||
                (s_eps[i].profile_id != ep->profile_id) ||
                (s_eps[i].device_id != ep->device_id) ||
                (s_eps[i].in_cluster_count != ep->in_cluster_count) ||
                (s_eps[i].out_cluster_count != ep->out_cluster_count) ||
                (memcmp(s_eps[i].in_clusters, ep->in_clusters, sizeof(s_eps[i].in_clusters)) != 0) ||
                (memcmp(s_eps[i].out_clusters, ep->out_clusters, sizeof(s_eps[i].out_clusters)) != 0);
            s_eps[i] = *ep;
            if (changed) {
                s_eps[i].version = ++s_version_seq;
            }
            return ESP_OK;
        }
    }

    if (s_ep_count >= (sizeof(s_eps) / sizeof(s_eps[0]))) {
        return ESP_ERR_NO_MEM;
    }

    s_eps[s_ep_count] = *ep;
    s_eps[s_ep_count].version = ++s_version_seq;
    s_ep_count++;
    
    // Auto-sync to persistent storage when new endpoint is discovered
    (void)gw_device_registry_sync_endpoints(&ep->uid);
    return ESP_OK;
}

esp_err_t gw_zb_model_remove_device(const gw_device_uid_t *uid)
{
    if (!s_inited || uid == NULL || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t write_idx = 0;
    for (size_t i = 0; i < s_ep_count; i++) {
        if (!uid_equals(&s_eps[i].uid, uid)) {
            if (write_idx != i) {
                s_eps[write_idx] = s_eps[i];
            }
            write_idx++;
        }
    }
    for (size_t i = write_idx; i < s_ep_count; i++) {
        memset(&s_eps[i], 0, sizeof(s_eps[i]));
    }
    s_ep_count = write_idx;
    return ESP_OK;
}

size_t gw_zb_model_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    if (!s_inited || uid == NULL || out_eps == NULL || max_eps == 0) {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < s_ep_count && written < max_eps; i++) {
        if (uid_equals(&s_eps[i].uid, uid)) {
            out_eps[written++] = s_eps[i];
        }
    }
    return written;
}

bool gw_zb_model_find_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid)
{
    if (!s_inited || out_uid == NULL) {
        return false;
    }

    for (size_t i = 0; i < s_ep_count; i++) {
        if (s_eps[i].short_addr == short_addr && s_eps[i].uid.uid[0] != '\0') {
            *out_uid = s_eps[i].uid;
            return true;
        }
    }
    return false;
}
