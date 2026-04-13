#include "gw_core/rules_engine.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "gw_core/action_exec.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_proto/gw_proto_types.h"
#include "gw_model/gw_model_automation.h"
#include "gw_model/gw_model_state.h"

static const char *TAG = "gw_rules";

#define GW_AUTOMATION_CAP 32
#define GW_RULES_EVENT_Q_CAP 96
#define GW_RULES_TASK_PRIO 7

#define GW_RULE_INDEX_CAP 256

typedef struct {
    uint8_t evt_type;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint32_t uid_hash;
    uint32_t cmd_hash;
    uint8_t has_uid;
    uint8_t has_endpoint;
    uint8_t has_cluster;
    uint8_t has_attr;
    uint8_t has_cmd;
} trigger_key_t;

typedef struct {
    bool used;
    trigger_key_t key;
    uint32_t auto_mask;
} trigger_index_slot_t;

typedef struct {
    gw_automation_entry_t autos[GW_AUTOMATION_CAP];
    size_t count;
    trigger_index_slot_t index[GW_RULE_INDEX_CAP];
} rules_cache_t;

static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static rules_cache_t s_cache_a;
EXT_RAM_BSS_ATTR static rules_cache_t s_cache_b;
static rules_cache_t *s_cache = &s_cache_a;
static bool s_cache_use_a = true;

static bool s_inited;
static QueueHandle_t s_q;
static bool s_q_caps_alloc;
static TaskHandle_t s_task;
static uint32_t s_trace_id;

static const char *strtab_at(const gw_automation_entry_t *entry, uint32_t off)
{
    if (!entry) return "";
    if (off == 0) return "";
    if (off >= entry->string_table_size) return "";
    return entry->string_table + off;
}

static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) {
        return h;
    }
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

static bool trigger_key_equals(const trigger_key_t *a, const trigger_key_t *b)
{
    return a->evt_type == b->evt_type &&
           a->endpoint == b->endpoint &&
           a->cluster_id == b->cluster_id &&
           a->attr_id == b->attr_id &&
           a->uid_hash == b->uid_hash &&
           a->cmd_hash == b->cmd_hash &&
           a->has_uid == b->has_uid &&
           a->has_endpoint == b->has_endpoint &&
           a->has_cluster == b->has_cluster &&
           a->has_attr == b->has_attr &&
           a->has_cmd == b->has_cmd;
}

static uint32_t trigger_key_hash(const trigger_key_t *k)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof(*k); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void trigger_index_clear(rules_cache_t *cache)
{
    memset(cache->index, 0, sizeof(cache->index));
}

static void trigger_index_insert(rules_cache_t *cache, const trigger_key_t *key, uint8_t auto_idx)
{
    if (!cache || !key || auto_idx >= 32) {
        return;
    }

    uint32_t h = trigger_key_hash(key);
    uint32_t pos = h & (GW_RULE_INDEX_CAP - 1);
    for (size_t i = 0; i < GW_RULE_INDEX_CAP; i++) {
        trigger_index_slot_t *slot = &cache->index[pos];
        if (!slot->used) {
            slot->used = true;
            slot->key = *key;
            slot->auto_mask = (1u << auto_idx);
            return;
        }
        if (trigger_key_equals(&slot->key, key)) {
            slot->auto_mask |= (1u << auto_idx);
            return;
        }
        pos = (pos + 1u) & (GW_RULE_INDEX_CAP - 1);
    }

    // Should never happen with current caps, but keep deterministic behavior.
    ESP_LOGW(TAG, "trigger index full, auto_idx=%u dropped", (unsigned)auto_idx);
}

static uint32_t trigger_index_lookup(const rules_cache_t *cache, const trigger_key_t *key)
{
    if (!cache || !key) {
        return 0;
    }

    uint32_t h = trigger_key_hash(key);
    uint32_t pos = h & (GW_RULE_INDEX_CAP - 1);
    for (size_t i = 0; i < GW_RULE_INDEX_CAP; i++) {
        const trigger_index_slot_t *slot = &cache->index[pos];
        if (!slot->used) {
            return 0;
        }
        if (trigger_key_equals(&slot->key, key)) {
            return slot->auto_mask;
        }
        pos = (pos + 1u) & (GW_RULE_INDEX_CAP - 1);
    }
    return 0;
}

static void publish_rules_fired(const gw_proto_event_v1_t *e, const char *automation_id)
{
    gw_proto_trace_v1_t trace = {0};
    trace.v = 1;
    trace.kind = GW_PROTO_TRACE_RULES_FIRED;
    trace.ok = 1;
    trace.id = ++s_trace_id;
    trace.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (e) {
        strlcpy(trace.device_uid, e->device_uid.uid, sizeof(trace.device_uid));
        trace.short_addr = e->short_addr;
    }
    strlcpy(trace.automation_id, automation_id ? automation_id : "", sizeof(trace.automation_id));
    const gw_proto_hdr_t hdr = {
        .version = GW_PROTO_VERSION_V1,
        .type = GW_PROTO_MSG_EVENT_TRACE,
        .len = sizeof(trace),
        .seq = 0,
        .reserved = 0,
    };
    (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_TRACE, &hdr, &trace);
}

static void publish_rules_action(const char *automation_id, size_t idx, bool ok, const char *err)
{
    gw_proto_trace_v1_t trace = {0};
    trace.v = 1;
    trace.kind = GW_PROTO_TRACE_RULES_ACTION;
    trace.ok = ok ? 1 : 0;
    trace.id = ++s_trace_id;
    trace.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    trace.action_index = (uint16_t)idx;
    strlcpy(trace.automation_id, automation_id ? automation_id : "", sizeof(trace.automation_id));
    strlcpy(trace.error_text, err ? err : "", sizeof(trace.error_text));
    const gw_proto_hdr_t hdr = {
        .version = GW_PROTO_VERSION_V1,
        .type = GW_PROTO_MSG_EVENT_TRACE,
        .len = sizeof(trace),
        .seq = 0,
        .reserved = 0,
    };
    (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_TRACE, &hdr, &trace);
}

typedef struct {
    uint8_t endpoint;
    bool has_endpoint;
    char cmd_buf[32];
    const char *cmd;
    bool has_cmd;
    uint16_t cluster_id;
    bool has_cluster;
    uint16_t attr_id;
    bool has_attr;
} event_payload_view_t;

static void build_payload_view_from_event(const gw_proto_event_v1_t *e, event_payload_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!e) return;

    if (e->endpoint > 0) {
        out->endpoint = e->endpoint;
        out->has_endpoint = true;
    }
    if (e->cmd[0]) {
        strlcpy(out->cmd_buf, e->cmd, sizeof(out->cmd_buf));
        out->cmd = out->cmd_buf;
        out->has_cmd = out->cmd_buf[0] != '\0';
    }
    if (e->cluster_id != 0) {
        out->cluster_id = e->cluster_id;
        out->has_cluster = true;
    }
    if (e->attr_id != 0) {
        out->attr_id = e->attr_id;
        out->has_attr = true;
    }
}

static gw_auto_evt_type_t evt_type_from_event(const gw_proto_event_v1_t *e)
{
    if (!e) {
        return 0;
    }
    switch ((gw_proto_event_id_t)e->event_id_kind) {
        case GW_PROTO_EVENT_COMMAND:
            return GW_AUTO_EVT_ZIGBEE_COMMAND;
        case GW_PROTO_EVENT_ATTR_REPORT:
            return GW_AUTO_EVT_ZIGBEE_ATTR_REPORT;
        case GW_PROTO_EVENT_DEVICE_JOIN:
            return GW_AUTO_EVT_DEVICE_JOIN;
        case GW_PROTO_EVENT_DEVICE_LEAVE:
            return GW_AUTO_EVT_DEVICE_LEAVE;
        default:
            return 0;
    }
}

static bool trigger_matches(const gw_automation_entry_t *entry,
                            const gw_auto_bin_trigger_v2_t *t,
                            gw_auto_evt_type_t evt_type,
                            const gw_proto_event_v1_t *e,
                            const event_payload_view_t *pv)
{
    if (t->event_type != evt_type) return false;
    if (t->device_uid_off && strcmp(strtab_at(entry, t->device_uid_off), e->device_uid.uid) != 0) return false;
    if (t->endpoint && (!pv->has_endpoint || pv->endpoint != t->endpoint)) return false;

    if (evt_type == GW_AUTO_EVT_ZIGBEE_COMMAND) {
        if (t->cmd_off && (!pv->has_cmd || strcmp(strtab_at(entry, t->cmd_off), pv->cmd) != 0)) return false;
        if (t->cluster_id && (!pv->has_cluster || pv->cluster_id != t->cluster_id)) return false;
    } else if (evt_type == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
        if (t->cluster_id && (!pv->has_cluster || pv->cluster_id != t->cluster_id)) return false;
        if (t->attr_id && (!pv->has_attr || pv->attr_id != t->attr_id)) return false;
    }
    return true;
}

static bool state_to_number_bool(const gw_proto_state_item_v1_t *s, double *out_n, bool *out_b)
{
    if (!s) return false;
    switch (s->value_type) {
        case GW_STATE_VALUE_BOOL:
            *out_n = s->value_bool ? 1.0 : 0.0;
            *out_b = s->value_bool;
            return true;
        case GW_STATE_VALUE_F32:
            *out_n = s->value_f32;
            *out_b = fabs(s->value_f32) > 1e-6;
            return true;
        case GW_STATE_VALUE_U32:
            *out_n = s->value_u32;
            *out_b = s->value_u32 != 0;
            return true;
        case GW_STATE_VALUE_U64:
            *out_n = s->value_u64;
            *out_b = s->value_u64 != 0;
            return true;
        default:
            return false;
    }
}

typedef struct {
    const char *key;
    bool found;
    gw_proto_state_item_v1_t best;
} state_lookup_any_ctx_t;

static bool find_state_any_cb(const void *record, void *user_ctx)
{
    const gw_proto_state_item_v1_t *st = (const gw_proto_state_item_v1_t *)record;
    state_lookup_any_ctx_t *ctx = (state_lookup_any_ctx_t *)user_ctx;

    if (!st || !ctx || !ctx->key) {
        return true;
    }

    if (strncmp(st->key, ctx->key, sizeof(st->key)) != 0) {
        return true;
    }

    if (!ctx->found || st->ts_ms > ctx->best.ts_ms || (st->ts_ms == ctx->best.ts_ms && st->version > ctx->best.version)) {
        ctx->best = *st;
        ctx->found = true;
    }

    return true;
}

static bool conditions_pass(const gw_automation_entry_t *entry)
{
    if (entry->conditions_count == 0) return true;

    for (uint8_t i = 0; i < entry->conditions_count; i++) {
        const gw_auto_bin_condition_v2_t *co = &entry->conditions[i];
        const char *uid_s = strtab_at(entry, co->device_uid_off);
        const char *key = strtab_at(entry, co->key_off);
        if (!uid_s[0] || !key[0]) return false;

        gw_device_uid_t uid = {0};
        strlcpy(uid.uid, uid_s, sizeof(uid.uid));
        gw_proto_state_item_v1_t st = {0};
        esp_err_t state_err = ESP_OK;
        if (co->endpoint > 0) {
            gw_model_state_key_t state_key = {0};
            state_key.uid = uid;
            state_key.endpoint = co->endpoint;
            strlcpy(state_key.key, key, sizeof(state_key.key));
            state_err = gw_model_get_state(&state_key, &st);
        } else {
            state_lookup_any_ctx_t ctx = {
                .key = key,
                .found = false,
            };
            (void)gw_model_iter_state_for_device(&uid, find_state_any_cb, &ctx);
            if (ctx.found) {
                st = ctx.best;
            } else {
                state_err = ESP_ERR_NOT_FOUND;
            }
        }
        if (state_err != ESP_OK) return false;

        double actual_n = 0;
        bool actual_b = false;
        if (!state_to_number_bool(&st, &actual_n, &actual_b)) return false;

        const gw_auto_op_t op = (gw_auto_op_t)co->op;
        if (co->val_type == GW_AUTO_VAL_BOOL) {
            bool exp = co->v.b != 0;
            if ((op == GW_AUTO_OP_EQ && actual_b != exp) || (op == GW_AUTO_OP_NE && actual_b == exp)) return false;
        } else {
            double exp = co->v.f64;
            double act = actual_n;
            if ((op == GW_AUTO_OP_EQ && fabs(act - exp) > 1e-6) ||
                (op == GW_AUTO_OP_NE && fabs(act - exp) < 1e-6) ||
                (op == GW_AUTO_OP_GT && act <= exp) ||
                (op == GW_AUTO_OP_LT && act >= exp) ||
                (op == GW_AUTO_OP_GE && act < exp) ||
                (op == GW_AUTO_OP_LE && act > exp)) {
                return false;
            }
        }
    }
    return true;
}

static void index_trigger(rules_cache_t *cache,
                          const gw_automation_entry_t *entry,
                          const gw_auto_bin_trigger_v2_t *t,
                          uint8_t auto_idx)
{
    trigger_key_t k = {0};
    k.evt_type = t->event_type;

    if (t->device_uid_off) {
        const char *uid = strtab_at(entry, t->device_uid_off);
        if (uid[0]) {
            k.has_uid = 1;
            k.uid_hash = fnv1a32(uid);
        }
    }
    if (t->endpoint) {
        k.has_endpoint = 1;
        k.endpoint = t->endpoint;
    }

    if (t->event_type == GW_AUTO_EVT_ZIGBEE_COMMAND) {
        if (t->cmd_off) {
            const char *cmd = strtab_at(entry, t->cmd_off);
            if (cmd[0]) {
                k.has_cmd = 1;
                k.cmd_hash = fnv1a32(cmd);
            }
        }
        if (t->cluster_id) {
            k.has_cluster = 1;
            k.cluster_id = t->cluster_id;
        }
    } else if (t->event_type == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
        if (t->cluster_id) {
            k.has_cluster = 1;
            k.cluster_id = t->cluster_id;
        }
        if (t->attr_id) {
            k.has_attr = 1;
            k.attr_id = t->attr_id;
        }
    }

    trigger_index_insert(cache, &k, auto_idx);
}

static void rebuild_trigger_index(rules_cache_t *cache)
{
    trigger_index_clear(cache);

    for (uint8_t i = 0; i < cache->count && i < 32; i++) {
        const gw_automation_entry_t *entry = &cache->autos[i];
        if (!entry->enabled) {
            continue;
        }
        for (uint8_t ti = 0; ti < entry->triggers_count; ti++) {
            index_trigger(cache, entry, &entry->triggers[ti], i);
        }
    }
}

static void reload_automation_cache(void)
{
    rules_cache_t *dst = s_cache_use_a ? &s_cache_b : &s_cache_a;
    memset(dst, 0, sizeof(*dst));
    dst->count = gw_model_list_automations(dst->autos, GW_AUTOMATION_CAP);
    rebuild_trigger_index(dst);

    portENTER_CRITICAL(&s_cache_lock);
    s_cache = dst;
    s_cache_use_a = !s_cache_use_a;
    portEXIT_CRITICAL(&s_cache_lock);
}

static uint32_t lookup_candidate_mask(const rules_cache_t *cache,
                                      const gw_proto_event_v1_t *e,
                                      const event_payload_view_t *pv,
                                      gw_auto_evt_type_t evt_type)
{
    if (!cache || !e) {
        return 0;
    }

    const bool ev_has_uid = e->device_uid.uid[0] != '\0';
    const uint32_t ev_uid_hash = ev_has_uid ? fnv1a32(e->device_uid.uid) : 0;

    uint32_t mask = 0;
    trigger_key_t k = {0};
    k.evt_type = evt_type;

    if (evt_type == GW_AUTO_EVT_ZIGBEE_COMMAND) {
        const bool has_uid = ev_has_uid;
        const bool has_ep = pv->has_endpoint;
        const bool has_cmd = pv->has_cmd && pv->cmd && pv->cmd[0];
        const bool has_cluster = pv->has_cluster;
        const uint32_t ev_cmd_hash = has_cmd ? fnv1a32(pv->cmd) : 0;

        for (uint8_t u = 0; u <= (has_uid ? 1 : 0); u++) {
            for (uint8_t ep = 0; ep <= (has_ep ? 1 : 0); ep++) {
                for (uint8_t c = 0; c <= (has_cmd ? 1 : 0); c++) {
                    for (uint8_t cl = 0; cl <= (has_cluster ? 1 : 0); cl++) {
                        memset(&k, 0, sizeof(k));
                        k.evt_type = evt_type;
                        if (u) {
                            k.has_uid = 1;
                            k.uid_hash = ev_uid_hash;
                        }
                        if (ep) {
                            k.has_endpoint = 1;
                            k.endpoint = pv->endpoint;
                        }
                        if (c) {
                            k.has_cmd = 1;
                            k.cmd_hash = ev_cmd_hash;
                        }
                        if (cl) {
                            k.has_cluster = 1;
                            k.cluster_id = pv->cluster_id;
                        }
                        mask |= trigger_index_lookup(cache, &k);
                    }
                }
            }
        }
    } else if (evt_type == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
        const bool has_uid = ev_has_uid;
        const bool has_ep = pv->has_endpoint;
        const bool has_cluster = pv->has_cluster;
        const bool has_attr = pv->has_attr;

        for (uint8_t u = 0; u <= (has_uid ? 1 : 0); u++) {
            for (uint8_t ep = 0; ep <= (has_ep ? 1 : 0); ep++) {
                for (uint8_t cl = 0; cl <= (has_cluster ? 1 : 0); cl++) {
                    for (uint8_t a = 0; a <= (has_attr ? 1 : 0); a++) {
                        memset(&k, 0, sizeof(k));
                        k.evt_type = evt_type;
                        if (u) {
                            k.has_uid = 1;
                            k.uid_hash = ev_uid_hash;
                        }
                        if (ep) {
                            k.has_endpoint = 1;
                            k.endpoint = pv->endpoint;
                        }
                        if (cl) {
                            k.has_cluster = 1;
                            k.cluster_id = pv->cluster_id;
                        }
                        if (a) {
                            k.has_attr = 1;
                            k.attr_id = pv->attr_id;
                        }
                        mask |= trigger_index_lookup(cache, &k);
                    }
                }
            }
        }
    } else {
        const bool has_uid = ev_has_uid;
        const bool has_ep = pv->has_endpoint;

        for (uint8_t u = 0; u <= (has_uid ? 1 : 0); u++) {
            for (uint8_t ep = 0; ep <= (has_ep ? 1 : 0); ep++) {
                memset(&k, 0, sizeof(k));
                k.evt_type = evt_type;
                if (u) {
                    k.has_uid = 1;
                    k.uid_hash = ev_uid_hash;
                }
                if (ep) {
                    k.has_endpoint = 1;
                    k.endpoint = pv->endpoint;
                }
                mask |= trigger_index_lookup(cache, &k);
            }
        }
    }

    return mask;
}

static void process_event(const gw_proto_event_v1_t *e)
{
    if (!e) {
        return;
    }

    const gw_auto_evt_type_t evt_type = evt_type_from_event(e);
    if (!evt_type) {
        return;
    }

    const rules_cache_t *cache = NULL;
    portENTER_CRITICAL(&s_cache_lock);
    cache = s_cache;
    portEXIT_CRITICAL(&s_cache_lock);

    if (!cache || cache->count == 0) {
        return;
    }

    event_payload_view_t pv;
    build_payload_view_from_event(e, &pv);

    uint32_t candidate_mask = lookup_candidate_mask(cache, e, &pv, evt_type);
    if (candidate_mask == 0) {
        return;
    }

    for (uint8_t i = 0; i < cache->count && i < 32; i++) {
        if ((candidate_mask & (1u << i)) == 0) {
            continue;
        }

        const gw_automation_entry_t *entry = &cache->autos[i];
        if (!entry->enabled) {
            continue;
        }

        bool matched = false;
        for (uint8_t ti = 0; ti < entry->triggers_count; ti++) {
            if (trigger_matches(entry, &entry->triggers[ti], evt_type, e, &pv)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (!conditions_pass(entry)) {
            ESP_LOGI(TAG, "rules candidate auto[%u] id=%s blocked by conditions", (unsigned)i, entry->id);
            continue;
        }

        ESP_LOGI(TAG, "rules fire auto[%u] id=%s", (unsigned)i, entry->id);
        publish_rules_fired(e, entry->id);

        for (uint8_t ai = 0; ai < entry->actions_count; ai++) {
            char errbuf[96] = {0};
            esp_err_t rc = gw_action_exec_action(entry, &entry->actions[ai], errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                publish_rules_action(entry->id, ai, false, errbuf[0] ? errbuf : "exec failed");
                break;
            }
            publish_rules_action(entry->id, ai, true, NULL);
        }
    }
}

static void rules_task(void *arg)
{
    gw_proto_event_v1_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) == pdTRUE) {
            process_event(&e);
        }
    }
}

static void rules_proto_listener(gw_proto_bus_channel_t channel, const gw_proto_hdr_t *hdr, const void *payload, void *user_ctx)
{
    (void)user_ctx;
    if (!s_inited || !hdr || !payload || hdr->len == 0) {
        return;
    }

    if (channel == GW_PROTO_BUS_CHANNEL_MODEL) {
        if (hdr->type == GW_PROTO_MSG_AUTOMATION_UPSERT || hdr->type == GW_PROTO_MSG_AUTOMATION_REMOVE) {
            reload_automation_cache();
        }
        return;
    }

    if (!s_q || channel != GW_PROTO_BUS_CHANNEL_INGRESS || hdr->type != GW_PROTO_MSG_EVENT_ZB) {
        return;
    }
    if (hdr->len < sizeof(gw_proto_event_v1_t)) {
        return;
    }

    const gw_proto_event_v1_t *event = (const gw_proto_event_v1_t *)payload;

    if (xQueueSend(s_q, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rules event queue overflow");
    }
}

esp_err_t gw_rules_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_q_caps_alloc = false;
    s_q = xQueueCreateWithCaps(GW_RULES_EVENT_Q_CAP, sizeof(gw_proto_event_v1_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_q) {
        s_q_caps_alloc = true;
    }
    if (!s_q) {
        s_q = xQueueCreateWithCaps(GW_RULES_EVENT_Q_CAP, sizeof(gw_proto_event_v1_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_q) {
            s_q_caps_alloc = true;
        }
    }
    if (!s_q) {
        s_q = xQueueCreate(GW_RULES_EVENT_Q_CAP, sizeof(gw_proto_event_v1_t));
        s_q_caps_alloc = false;
    }
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreateWithCaps(rules_task,
                                             "rules",
                                             4096,
                                             NULL,
                                             GW_RULES_TASK_PRIO,
                                             &s_task,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        task_ok = xTaskCreateWithCaps(rules_task,
                                      "rules",
                                      4096,
                                      NULL,
                                      GW_RULES_TASK_PRIO,
                                      &s_task,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (task_ok != pdPASS) {
        if (s_q_caps_alloc) {
            vQueueDeleteWithCaps(s_q);
        } else {
            vQueueDelete(s_q);
        }
        s_q = NULL;
        s_q_caps_alloc = false;
        return ESP_FAIL;
    }

    (void)gw_proto_bus_add_listener(rules_proto_listener, GW_PROTO_BUS_CHANNEL_INGRESS | GW_PROTO_BUS_CHANNEL_MODEL, NULL);
    reload_automation_cache();

    s_inited = true;
    ESP_LOGI(TAG, "rules engine initialized (indexed)");
    return ESP_OK;
}
