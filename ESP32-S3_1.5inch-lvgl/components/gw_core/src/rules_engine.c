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

static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_automation_entry_t s_cache_a[GW_AUTOMATION_CAP];
static gw_automation_entry_t s_cache_b[GW_AUTOMATION_CAP];
static gw_automation_entry_t *s_cache = s_cache_a;
static size_t s_cache_count;
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
    if (strcmp(e->type, "zigbee.cmd") == 0) return GW_AUTO_EVT_ZIGBEE_COMMAND;
    if (strcmp(e->type, "zigbee.attr_report") == 0) return GW_AUTO_EVT_ZIGBEE_ATTR_REPORT;
    if (strcmp(e->type, "device.join") == 0) return GW_AUTO_EVT_DEVICE_JOIN;
    if (strcmp(e->type, "device.leave") == 0) return GW_AUTO_EVT_DEVICE_LEAVE;
    return 0;
}

static bool trigger_matches(const gw_automation_entry_t *entry, const gw_auto_bin_trigger_v2_t *t, gw_auto_evt_type_t evt_type, const gw_event_t *e, const event_payload_view_t *pv)
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
    case GW_STATE_VALUE_BOOL: *out_n = s->value_bool ? 1.0 : 0.0; *out_b = s->value_bool; return true;
    case GW_STATE_VALUE_F32:  *out_n = s->value_f32; *out_b = fabs(s->value_f32) > 1e-6; return true;
    case GW_STATE_VALUE_U32:  *out_n = s->value_u32; *out_b = s->value_u32 != 0; return true;
    case GW_STATE_VALUE_U64:  *out_n = s->value_u64; *out_b = s->value_u64 != 0; return true;
    default: return false;
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
            if ((op == GW_AUTO_OP_EQ && fabs(act - exp) > 1e-6) || (op == GW_AUTO_OP_NE && fabs(act - exp) < 1e-6) ||
                (op == GW_AUTO_OP_GT && act <= exp) || (op == GW_AUTO_OP_LT && act >= exp) ||
                (op == GW_AUTO_OP_GE && act < exp) || (op == GW_AUTO_OP_LE && act > exp)) return false;
        }
    }
    return true;
}

static void reload_automation_cache(void)
{
    gw_automation_entry_t *dst = s_cache_use_a ? s_cache_b : s_cache_a;
    size_t count = gw_automation_store_list(dst, GW_AUTOMATION_CAP);
    portENTER_CRITICAL(&s_cache_lock);
    s_cache = dst;
    s_cache_count = count;
    s_cache_use_a = !s_cache_use_a;
    portEXIT_CRITICAL(&s_cache_lock);
}

static void process_event(const gw_event_t *e)
{
    if (!e || !e->type[0] || strcmp(e->source, "rules") == 0) return;

    const gw_auto_evt_type_t evt_type = evt_type_from_event(e);
    if (!evt_type) return;

    gw_automation_entry_t *all_autos = NULL;
    size_t auto_count = 0;
    portENTER_CRITICAL(&s_cache_lock);
    all_autos = s_cache;
    auto_count = s_cache_count;
    portEXIT_CRITICAL(&s_cache_lock);

    if (auto_count == 0 || !all_autos) {
        return;
    }

    event_payload_view_t pv;
    build_payload_view_from_event(e, &pv);

    for (size_t i = 0; i < auto_count; i++) {
        const gw_automation_entry_t *entry = &all_autos[i];
        if (!entry->enabled) continue;

        bool matched = false;
        for (uint8_t ti = 0; ti < entry->triggers_count; ti++) {
            if (trigger_matches(entry, &entry->triggers[ti], evt_type, e, &pv)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        if (!conditions_pass(entry)) continue;

        publish_rules_fired(e, entry->id);

        for (uint8_t ai = 0; ai < entry->actions_count; ai++) {
            char errbuf[96] = {0};
            // This function needs to be adapted or wrapped to work with gw_automation_entry_t
            // For now, let's assume a wrapper exists. We will need to create it.
            // esp_err_t rc = gw_action_exec_compiled(c, &acts[ai], errbuf, sizeof(errbuf));
            // Let's create a temporary compatible structure for gw_action_exec_compiled
            gw_auto_compiled_t temp_compiled = {
                .strings = (char*)entry->string_table,
                .hdr.strings_size = entry->string_table_size
            };

            esp_err_t rc = gw_action_exec_compiled(&temp_compiled, &entry->actions[ai], errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                publish_rules_action(entry->id, ai, false, errbuf[0] ? errbuf : "exec failed");
                break; // Stop actions on first failure for this rule
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
    if (s_inited) return ESP_OK;

    s_q = xQueueCreate(GW_RULES_EVENT_Q_CAP, sizeof(gw_event_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    if (xTaskCreateWithCaps(rules_task, "rules", 4096, NULL, GW_RULES_TASK_PRIO, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(s_q);
        return ESP_FAIL;
    }

    gw_event_bus_add_listener(rules_event_listener, NULL);
    reload_automation_cache();

    s_inited = true;
    ESP_LOGI(TAG, "rules engine initialized");
    return ESP_OK;
}


