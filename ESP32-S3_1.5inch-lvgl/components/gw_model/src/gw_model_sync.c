#include "gw_model/gw_model_sync.h"

#include <string.h>

#include "gw_core/types.h"
#include "gw_core/model_types.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_model/gw_model_groups.h"
#include "gw_model/gw_model_state.h"
#include "gw_model/gw_model_topology.h"

static bool s_inited;
static bool s_snapshot_active;
static gw_device_uid_t s_snapshot_stale[GW_DEVICE_MAX_DEVICES];
static size_t s_snapshot_stale_count;

static bool resolve_uid(const gw_proto_event_v1_t *e, gw_device_uid_t *out_uid)
{
    if (!e || !out_uid) {
        return false;
    }

    memset(out_uid, 0, sizeof(*out_uid));
    if (e->device_uid.uid[0] != '\0') {
        *out_uid = e->device_uid;
        return true;
    }
    if (e->short_addr != 0 && gw_model_find_device_uid_by_short(e->short_addr, out_uid)) {
        return true;
    }
    return false;
}

static bool value_as_bool(const gw_proto_event_v1_t *e, bool *out)
{
    if (!e || !out) {
        return false;
    }
    switch ((gw_proto_event_value_type_t)e->value_type) {
        case GW_PROTO_EVENT_VALUE_BOOL:
            *out = (e->value_bool != 0);
            return true;
        case GW_PROTO_EVENT_VALUE_I64:
            *out = (e->value_i64 != 0);
            return true;
        case GW_PROTO_EVENT_VALUE_F32:
            *out = (e->value_f32 != 0.0f);
            return true;
        default:
            return false;
    }
}

static bool value_as_i64(const gw_proto_event_v1_t *e, int64_t *out)
{
    if (!e || !out) {
        return false;
    }
    switch ((gw_proto_event_value_type_t)e->value_type) {
        case GW_PROTO_EVENT_VALUE_I64:
            *out = e->value_i64;
            return true;
        case GW_PROTO_EVENT_VALUE_BOOL:
            *out = e->value_bool ? 1 : 0;
            return true;
        case GW_PROTO_EVENT_VALUE_F32:
            *out = (int64_t)e->value_f32;
            return true;
        default:
            return false;
    }
}

static void upsert_state_bool(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, bool value, uint64_t ts_ms)
{
    gw_model_state_key_t state_key = {0};
    state_key.uid = *uid;
    state_key.endpoint = endpoint;
    strlcpy(state_key.key, key, sizeof(state_key.key));

    gw_proto_state_item_v1_t record = {0};
    if (gw_model_get_state(&state_key, &record) != ESP_OK) {
        record.uid = *uid;
        record.endpoint = endpoint;
        strlcpy(record.key, key, sizeof(record.key));
        record.version = 1;
    } else {
        record.version++;
    }
    record.value_type = GW_STATE_VALUE_BOOL;
    record.value_bool = value ? 1 : 0;
    record.ts_ms = ts_ms;

    bool changed = false;
    bool inserted = false;
    (void)gw_model_upsert_state(&record, &changed, &inserted);
}

static void upsert_state_f32(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, float value, uint64_t ts_ms)
{
    gw_model_state_key_t state_key = {0};
    state_key.uid = *uid;
    state_key.endpoint = endpoint;
    strlcpy(state_key.key, key, sizeof(state_key.key));

    gw_proto_state_item_v1_t record = {0};
    if (gw_model_get_state(&state_key, &record) != ESP_OK) {
        record.uid = *uid;
        record.endpoint = endpoint;
        strlcpy(record.key, key, sizeof(record.key));
        record.version = 1;
    } else {
        record.version++;
    }
    record.value_type = GW_STATE_VALUE_F32;
    record.value_f32 = value;
    record.ts_ms = ts_ms;

    bool changed = false;
    bool inserted = false;
    (void)gw_model_upsert_state(&record, &changed, &inserted);
}

static void upsert_state_u32(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, uint32_t value, uint64_t ts_ms)
{
    gw_model_state_key_t state_key = {0};
    state_key.uid = *uid;
    state_key.endpoint = endpoint;
    strlcpy(state_key.key, key, sizeof(state_key.key));

    gw_proto_state_item_v1_t record = {0};
    if (gw_model_get_state(&state_key, &record) != ESP_OK) {
        record.uid = *uid;
        record.endpoint = endpoint;
        strlcpy(record.key, key, sizeof(record.key));
        record.version = 1;
    } else {
        record.version++;
    }
    record.value_type = GW_STATE_VALUE_U32;
    record.value_u32 = value;
    record.ts_ms = ts_ms;

    bool changed = false;
    bool inserted = false;
    (void)gw_model_upsert_state(&record, &changed, &inserted);
}

static void upsert_state_u64(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, uint64_t value, uint64_t ts_ms)
{
    gw_model_state_key_t state_key = {0};
    state_key.uid = *uid;
    state_key.endpoint = endpoint;
    strlcpy(state_key.key, key, sizeof(state_key.key));

    gw_proto_state_item_v1_t record = {0};
    if (gw_model_get_state(&state_key, &record) != ESP_OK) {
        record.uid = *uid;
        record.endpoint = endpoint;
        strlcpy(record.key, key, sizeof(record.key));
        record.version = 1;
    } else {
        record.version++;
    }
    record.value_type = GW_STATE_VALUE_U64;
    record.value_u64 = value;
    record.ts_ms = ts_ms;

    bool changed = false;
    bool inserted = false;
    (void)gw_model_upsert_state(&record, &changed, &inserted);
}

static void process_attr_report(const gw_device_uid_t *uid, const gw_proto_event_v1_t *e)
{
    if (!uid || !e || uid->uid[0] == '\0' || e->cluster_id == 0) {
        return;
    }

    const uint16_t cluster = e->cluster_id;
    const uint16_t attr = e->attr_id;
    const uint8_t endpoint = e->endpoint;

    if (cluster == 0x0402 && attr == 0x0000) {
        if ((gw_proto_event_value_type_t)e->value_type == GW_PROTO_EVENT_VALUE_F32) {
            upsert_state_f32(uid, endpoint, "temperature_c", e->value_f32, e->ts_ms);
        } else {
            int64_t raw = 0;
            if (value_as_i64(e, &raw)) {
                upsert_state_f32(uid, endpoint, "temperature_c", ((float)raw) / 100.0f, e->ts_ms);
            }
        }
        return;
    }

    if (cluster == 0x0405 && attr == 0x0000) {
        if ((gw_proto_event_value_type_t)e->value_type == GW_PROTO_EVENT_VALUE_F32) {
            upsert_state_f32(uid, endpoint, "humidity_pct", e->value_f32, e->ts_ms);
        } else {
            int64_t raw = 0;
            if (value_as_i64(e, &raw) && raw >= 0) {
                upsert_state_f32(uid, endpoint, "humidity_pct", ((float)raw) / 100.0f, e->ts_ms);
            }
        }
        return;
    }

    if (cluster == 0x0001 && attr == 0x0021) {
        int64_t pct = 0;
        if (value_as_i64(e, &pct) && pct >= 0) {
            upsert_state_u32(uid, endpoint, "battery_pct", (uint32_t)pct, e->ts_ms);
        }
        return;
    }

    if (cluster == 0x0001 && attr == 0x0020) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw) && raw >= 0) {
            upsert_state_u32(uid, endpoint, "battery_mv", (uint32_t)raw, e->ts_ms);
        }
        return;
    }

    if (cluster == 0x0006 && attr == 0x0000) {
        bool onoff = false;
        if (value_as_bool(e, &onoff)) {
            upsert_state_bool(uid, endpoint, "onoff", onoff, e->ts_ms);
        }
        return;
    }

    if (cluster == 0x0008 && attr == 0x0000) {
        int64_t lvl = 0;
        if (value_as_i64(e, &lvl) && lvl >= 0) {
            upsert_state_u32(uid, endpoint, "level", (uint32_t)lvl, e->ts_ms);
        }
        return;
    }

    if (cluster == 0x0300 && (attr == 0x0003 || attr == 0x0004 || attr == 0x0007)) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw) && raw >= 0) {
            if (attr == 0x0003) {
                upsert_state_u32(uid, endpoint, "color_x", (uint32_t)raw, e->ts_ms);
            } else if (attr == 0x0004) {
                upsert_state_u32(uid, endpoint, "color_y", (uint32_t)raw, e->ts_ms);
            } else {
                upsert_state_u32(uid, endpoint, "color_temp_mireds", (uint32_t)raw, e->ts_ms);
            }
        }
        return;
    }

    if (cluster == 0x0406 && attr == 0x0000) {
        bool occ = false;
        if (value_as_bool(e, &occ)) {
            upsert_state_bool(uid, endpoint, "occupancy", occ, e->ts_ms);
        }
        return;
    }

    if (cluster == 0x0400 && attr == 0x0000) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw) && raw >= 0) {
            upsert_state_u32(uid, endpoint, "illuminance_raw", (uint32_t)raw, e->ts_ms);
        }
        return;
    }

    if (cluster == 0x0403 && attr == 0x0000) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw)) {
            upsert_state_f32(uid, endpoint, "pressure_raw", (float)raw, e->ts_ms);
        }
        return;
    }

    char key[40] = {0};
    (void)snprintf(key, sizeof(key), "cluster_%04x_attr_%04x", (unsigned)cluster, (unsigned)attr);
    switch ((gw_proto_event_value_type_t)e->value_type) {
        case GW_PROTO_EVENT_VALUE_BOOL:
            upsert_state_bool(uid, endpoint, key, e->value_bool != 0, e->ts_ms);
            break;
        case GW_PROTO_EVENT_VALUE_F32:
            upsert_state_f32(uid, endpoint, key, e->value_f32, e->ts_ms);
            break;
        case GW_PROTO_EVENT_VALUE_I64:
            if (e->value_i64 >= 0) {
                upsert_state_u64(uid, endpoint, key, (uint64_t)e->value_i64, e->ts_ms);
            } else {
                upsert_state_f32(uid, endpoint, key, (float)e->value_i64, e->ts_ms);
            }
            break;
        default:
            break;
    }
}

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (!a || !b) {
        return false;
    }
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void snapshot_stale_add_uid(const gw_device_uid_t *uid)
{
    if (!uid || uid->uid[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < s_snapshot_stale_count; ++i) {
        if (uid_equals(&s_snapshot_stale[i], uid)) {
            return;
        }
    }

    if (s_snapshot_stale_count < GW_DEVICE_MAX_DEVICES) {
        s_snapshot_stale[s_snapshot_stale_count++] = *uid;
    }
}

static void snapshot_stale_remove_uid(const gw_device_uid_t *uid)
{
    if (!uid || uid->uid[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < s_snapshot_stale_count; ++i) {
        if (uid_equals(&s_snapshot_stale[i], uid)) {
            for (size_t j = i + 1; j < s_snapshot_stale_count; ++j) {
                s_snapshot_stale[j - 1] = s_snapshot_stale[j];
            }
            s_snapshot_stale_count--;
            return;
        }
    }
}

static void snapshot_begin(const gw_proto_sync_begin_v1_t *msg)
{
    if (!msg) {
        return;
    }

    if (msg->scope != GW_PROTO_SYNC_SCOPE_FULL && msg->scope != GW_PROTO_SYNC_SCOPE_DEVICES) {
        return;
    }

    s_snapshot_stale_count = 0;
    const size_t device_count = gw_model_count_devices();
    for (size_t i = 0; i < device_count && s_snapshot_stale_count < GW_DEVICE_MAX_DEVICES; ++i) {
        gw_proto_device_v1_t record = {0};
        if (gw_model_get_device_by_index(i, &record) == ESP_OK) {
            snapshot_stale_add_uid(&record.device_uid);
        }
    }

    const size_t endpoint_count = gw_model_count_endpoints();
    for (size_t i = 0; i < endpoint_count && s_snapshot_stale_count < GW_DEVICE_MAX_DEVICES; ++i) {
        gw_proto_endpoint_v1_t record = {0};
        if (gw_model_get_endpoint_by_index(i, &record) == ESP_OK) {
            snapshot_stale_add_uid(&record.uid);
        }
    }

    const size_t state_count = gw_model_count_state();
    for (size_t i = 0; i < state_count && s_snapshot_stale_count < GW_DEVICE_MAX_DEVICES; ++i) {
        gw_proto_state_item_v1_t record = {0};
        if (gw_model_get_state_by_index(i, &record) == ESP_OK) {
            snapshot_stale_add_uid(&record.uid);
        }
    }
    s_snapshot_active = true;
}

static void snapshot_end(const gw_proto_sync_end_v1_t *msg)
{
    if (!msg || !s_snapshot_active) {
        return;
    }

    if (msg->scope != GW_PROTO_SYNC_SCOPE_FULL && msg->scope != GW_PROTO_SYNC_SCOPE_DEVICES) {
        return;
    }

    for (size_t i = 0; i < s_snapshot_stale_count; ++i) {
        bool removed = false;
        (void)gw_model_remove_full_device(&s_snapshot_stale[i], &removed);
    }
    s_snapshot_stale_count = 0;
    s_snapshot_active = false;
}

static void gw_model_bus_listener(gw_proto_bus_channel_t channel,
                                  const gw_proto_hdr_t *hdr,
                                  const void *payload,
                                  void *user_ctx)
{
    (void)channel;
    (void)user_ctx;

    if (!hdr || !payload) {
        return;
    }

    bool changed = false;
    bool inserted = false;
    bool removed = false;

    switch ((gw_proto_msg_type_t)hdr->type) {
        case GW_PROTO_MSG_EVENT_ZB:
            if (hdr->len >= sizeof(gw_proto_event_v1_t)) {
                const gw_proto_event_v1_t *event = (const gw_proto_event_v1_t *)payload;
                gw_device_uid_t uid = {0};
                const bool have_uid = resolve_uid(event, &uid);
                if (have_uid && event->event_id_kind == GW_PROTO_EVENT_ATTR_REPORT) {
                    process_attr_report(&uid, event);
                }
            }
            break;
        case GW_PROTO_MSG_SYNC_BEGIN:
            if (hdr->len >= sizeof(gw_proto_sync_begin_v1_t)) {
                snapshot_begin((const gw_proto_sync_begin_v1_t *)payload);
            }
            break;
        case GW_PROTO_MSG_SYNC_END:
            if (hdr->len >= sizeof(gw_proto_sync_end_v1_t)) {
                snapshot_end((const gw_proto_sync_end_v1_t *)payload);
            }
            break;
        case GW_PROTO_MSG_DEVICE_UPSERT:
            if (hdr->len >= sizeof(gw_proto_device_v1_t)) {
                (void)gw_model_upsert_device((const gw_proto_device_v1_t *)payload, &changed, &inserted);
                if (s_snapshot_active) {
                    const gw_proto_device_v1_t *msg = (const gw_proto_device_v1_t *)payload;
                    snapshot_stale_remove_uid(&msg->device_uid);
                }
            }
            break;
        case GW_PROTO_MSG_DEVICE_REMOVE:
            if (hdr->len >= sizeof(gw_proto_device_remove_v1_t)) {
                const gw_proto_device_remove_v1_t *msg = (const gw_proto_device_remove_v1_t *)payload;
                (void)gw_model_remove_full_device(&msg->device_uid, &removed);
                if (s_snapshot_active) {
                    snapshot_stale_remove_uid(&msg->device_uid);
                }
            }
            break;
        case GW_PROTO_MSG_ENDPOINT_UPSERT:
            if (hdr->len >= sizeof(gw_proto_endpoint_v1_t)) {
                (void)gw_model_upsert_endpoint((const gw_proto_endpoint_v1_t *)payload, &changed, &inserted);
            }
            break;
        case GW_PROTO_MSG_ENDPOINT_REMOVE:
            if (hdr->len >= sizeof(gw_proto_endpoint_remove_v1_t)) {
                const gw_proto_endpoint_remove_v1_t *msg = (const gw_proto_endpoint_remove_v1_t *)payload;
                gw_model_endpoint_key_t key = {
                    .uid = msg->uid,
                    .endpoint = msg->endpoint,
                };
                (void)gw_model_remove_endpoint(&key, &removed);
            }
            break;
        case GW_PROTO_MSG_STATE_ITEM:
            if (hdr->len >= sizeof(gw_proto_state_item_v1_t)) {
                (void)gw_model_upsert_state((const gw_proto_state_item_v1_t *)payload, &changed, &inserted);
            }
            break;
        case GW_PROTO_MSG_STATE_REMOVE:
            if (hdr->len >= sizeof(gw_proto_state_remove_v1_t)) {
                const gw_proto_state_remove_v1_t *msg = (const gw_proto_state_remove_v1_t *)payload;
                gw_model_state_key_t key = {0};
                key.uid = msg->uid;
                key.endpoint = msg->endpoint;
                memcpy(key.key, msg->key, sizeof(key.key));
                (void)gw_model_remove_state(&key, &removed);
            }
            break;
        case GW_PROTO_MSG_GROUP_UPSERT:
            if (hdr->len >= sizeof(gw_proto_group_v1_t)) {
                (void)gw_model_upsert_group((const gw_proto_group_v1_t *)payload, &changed, &inserted);
            }
            break;
        case GW_PROTO_MSG_GROUP_REMOVE:
            if (hdr->len >= sizeof(gw_proto_group_remove_v1_t)) {
                const gw_proto_group_remove_v1_t *msg = (const gw_proto_group_remove_v1_t *)payload;
                (void)gw_model_remove_group(msg->id, &removed);
            }
            break;
        case GW_PROTO_MSG_GROUP_ITEM_UPSERT:
            if (hdr->len >= sizeof(gw_proto_group_item_v1_t)) {
                (void)gw_model_upsert_group_item((const gw_proto_group_item_v1_t *)payload, &changed, &inserted);
            }
            break;
        case GW_PROTO_MSG_GROUP_ITEM_REMOVE:
            if (hdr->len >= sizeof(gw_proto_group_item_remove_v1_t)) {
                const gw_proto_group_item_remove_v1_t *msg = (const gw_proto_group_item_remove_v1_t *)payload;
                gw_model_endpoint_key_t key = {
                    .uid = msg->device_uid,
                    .endpoint = msg->endpoint,
                };
                (void)gw_model_remove_group_item(&key, &removed);
            }
            break;
        default:
            break;
    }
}

esp_err_t gw_model_sync_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t err = gw_proto_bus_add_listener(gw_model_bus_listener, GW_PROTO_BUS_CHANNEL_INGRESS, NULL);
    if (err == ESP_OK) {
        s_inited = true;
    }
    return err;
}

esp_err_t gw_model_sync_deinit(void)
{
    if (!s_inited) {
        return ESP_OK;
    }

    s_inited = false;
    return gw_proto_bus_remove_listener(gw_model_bus_listener, NULL);
}
