#include "gw_core/rules_engine.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_core/action_exec.h"
#include "gw_core/automation_store.h"
#include "gw_core/event_bus.h"
#include "gw_core/state_store.h"
#include "gw_core/types.h"

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
static rules_cache_t s_cache_a;
static rules_cache_t s_cache_b;
static rules_cache_t *s_cache = &s_cache_a;
static bool s_cache_use_a = true;

static bool s_inited;
static QueueHandle_t s_q;
static TaskHandle_t s_task;

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

static void publish_rules_fired(const gw_event_t *e, const char *automation_id)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "automation_id=%s", automation_id ? automation_id : "");
    gw_event_bus_publish("rules.fired", "rules", e ? e->device_uid : "", e ? e->short_addr : 0, msg);
}

static void publish_rules_action(const char *automation_id, size_t idx, bool ok, const char *err)
{
    char msg[192];
    if (err) {
        snprintf(msg, sizeof(msg), "automation_id=%s idx=%u ok=0 err=%s", automation_id, (unsigned)idx, err);
    } else {
        snprintf(msg, sizeof(msg), "automation_id=%s idx=%u ok=1", automation_id, (unsigned)idx);
    }
    gw_event_bus_publish("rules.action", "rules", "", 0, msg);
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

static void build_payload_view_from_event(const gw_event_t *e, event_payload_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!e) return;

    if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) {
        out->endpoint = e->payload_endpoint;
        out->has_endpoint = true;
    }
    if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_CMD) {
        strlcpy(out->cmd_buf, e->payload_cmd, sizeof(out->cmd_buf));
        out->cmd = out->cmd_buf;
        out->has_cmd = out->cmd_buf[0] != '\0';
    }
    if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) {
        out->cluster_id = e->payload_cluster;
        out->has_cluster = true;
    }
    if (e->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR) {
        out->attr_id = e->payload_attr;
        out->has_attr = true;
    }
}

static gw_auto_evt_type_t evt_type_from_event(const gw_event_t *e)
{
    if (strcmp(e->type, "zigbee.command") == 0) return GW_AUTO_EVT_ZIGBEE_COMMAND;
    if (strcmp(e->type, "zigbee.attr_report") == 0) return GW_AUTO_EVT_ZIGBEE_ATTR_REPORT;
    if (strcmp(e->type, "device.join") == 0) return GW_AUTO_EVT_DEVICE_JOIN;
    if (strcmp(e->type, "device.leave") == 0) return GW_AUTO_EVT_DEVICE_LEAVE;
    return 0;
}

static bool trigger_matches(const gw_automation_entry_t *entry,
                            const gw_auto_bin_trigger_v2_t *t,
                            gw_auto_evt_type_t evt_type,
                            const gw_event_t *e,
                            const event_payload_view_t *pv)
{
    if (t->event_type != evt_type) return false;
    if (t->device_uid_off && strcmp(strtab_at(entry, t->device_uid_off), e->device_uid) != 0) return false;
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

static bool state_to_number_bool(const gw_state_item_t *s, double *out_n, bool *out_b)
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
        gw_state_item_t st = {0};
        if (gw_state_store_get(&uid, key, &st) != ESP_OK) return false;

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
    dst->count = gw_automation_store_list(dst->autos, GW_AUTOMATION_CAP);
    rebuild_trigger_index(dst);

    portENTER_CRITICAL(&s_cache_lock);
    s_cache = dst;
    s_cache_use_a = !s_cache_use_a;
    portEXIT_CRITICAL(&s_cache_lock);
}

static uint32_t lookup_candidate_mask(const rules_cache_t *cache,
                                      const gw_event_t *e,
                                      const event_payload_view_t *pv,
                                      gw_auto_evt_type_t evt_type)
{
    if (!cache || !e) {
        return 0;
    }

    const bool ev_has_uid = e->device_uid[0] != '\0';
    const uint32_t ev_uid_hash = ev_has_uid ? fnv1a32(e->device_uid) : 0;

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

static void process_event(const gw_event_t *e)
{
    if (!e || !e->type[0] || strcmp(e->source, "rules") == 0) {
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
            continue;
        }

        publish_rules_fired(e, entry->id);

        for (uint8_t ai = 0; ai < entry->actions_count; ai++) {
            char errbuf[96] = {0};
            gw_auto_compiled_t temp_compiled = {
                .strings = (char *)entry->string_table,
                .hdr.strings_size = entry->string_table_size,
            };

            esp_err_t rc = gw_action_exec_compiled(&temp_compiled, &entry->actions[ai], errbuf, sizeof(errbuf));
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
    gw_event_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) == pdTRUE) {
            process_event(&e);
        }
    }
}

static void rules_event_listener(const gw_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event && (strcmp(event->type, "automation_saved") == 0 ||
                  strcmp(event->type, "automation_removed") == 0 ||
                  strcmp(event->type, "automation_enabled") == 0)) {
        reload_automation_cache();
    }

    if (s_inited && s_q && event) {
        if (xQueueSend(s_q, event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "rules event queue overflow");
        }
    }
}

esp_err_t gw_rules_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_q = xQueueCreate(GW_RULES_EVENT_Q_CAP, sizeof(gw_event_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreateWithCaps(rules_task,
                            "rules",
                            4096,
                            NULL,
                            GW_RULES_TASK_PRIO,
                            &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(s_q);
        return ESP_FAIL;
    }

    gw_event_bus_add_listener(rules_event_listener, NULL);
    reload_automation_cache();

    s_inited = true;
    ESP_LOGI(TAG, "rules engine initialized (indexed)");
    return ESP_OK;
}
