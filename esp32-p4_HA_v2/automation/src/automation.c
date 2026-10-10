#include "automation/automation.h"

#include <stddef.h>
#include <string.h>

#include "automation/automation_rule_sem.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_rule.h"
#include "ha_model/ha_state.h"
#include "semantics/semantics.h"

/*
 * Задача сервиса — единственный, кто читает Domain и постит команды от лица Automation.
 * try_push только копирует факт в inbox (DOMAIN_API.md §9): мутирующий API там нельзя.
 *
 * A3: правила хранятся как ha_rule_t (entity-адресованные). Триггеры: EVENT (машина
 * событий HA_ENTITY_ENTITY), STATE (canonical HA_ENTITY_PROPERTY_STATE), TIME (canonical
 * SYSTEM-свойства). Действие — semantic-команда HA_CMD_PROPERTY. uid/endpoint/cluster/attr
 * здесь неизвестны.
 */

#define AUTOMATION_INBOX_LENGTH 32
#define AUTOMATION_TASK_STACK 4096
#define AUTOMATION_TASK_PRIORITY 5

static const char *TAG = "automation";

static domain_t *s_domain;
static QueueHandle_t s_inbox;

static bool automation_accept(const domain_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_inbox == NULL) {
        return false;
    }
    return xQueueSend(s_inbox, event, 0) == pdTRUE;
}

/* Одному факту может соответствовать несколько правил; собираем совпадения до условий. */
#define AUTOMATION_FIRE_MAX 8

typedef struct {
    uint64_t rule_id;
    ha_rule_t rule;
} automation_match_t;

typedef struct {
    ha_entity_id_t source_entity; /* источник триггера (для резолва SOURCE) */

    bool is_event;
    ha_event_id_t event_id;

    bool is_time;
    uint16_t minutes_of_day;
    uint8_t weekday_mask;

    bool is_state;
    ha_entity_id_t state_entity;
    ha_property_id_t state_property;
    double state_value;
    bool state_prev_known;
    double state_prev_value;

    automation_match_t matches[AUTOMATION_FIRE_MAX];
    size_t count;
} automation_scan_t;

/* Кэш последних значений свойств — нужен, чтобы отличать «стало истинно/ложно». */
#define AUTOMATION_STATE_CACHE 64

typedef struct {
    bool used;
    ha_state_key_t key;
    double value;
} state_cache_slot_t;

static state_cache_slot_t s_state_cache[AUTOMATION_STATE_CACHE];

static bool state_cache_take(const ha_state_key_t *key, bool *known, double *prev)
{
    for (size_t i = 0; i < AUTOMATION_STATE_CACHE; i++) {
        if (s_state_cache[i].used && memcmp(&s_state_cache[i].key, key, sizeof(*key)) == 0) {
            *known = true;
            *prev = s_state_cache[i].value;
            return true;
        }
    }
    *known = false;
    return false; /* не найдено — не ошибка, просто первое наблюдение */
}

static void state_cache_put(const ha_state_key_t *key, double value)
{
    size_t free_slot = AUTOMATION_STATE_CACHE;
    for (size_t i = 0; i < AUTOMATION_STATE_CACHE; i++) {
        if (s_state_cache[i].used && memcmp(&s_state_cache[i].key, key, sizeof(*key)) == 0) {
            s_state_cache[i].value = value;
            return;
        }
        if (!s_state_cache[i].used && free_slot == AUTOMATION_STATE_CACHE) {
            free_slot = i;
        }
    }
    if (free_slot == AUTOMATION_STATE_CACHE) {
        free_slot = 0; /* кэш полон — вытесняем слот 0 (значение перечитается) */
    }
    s_state_cache[free_slot].used = true;
    s_state_cache[free_slot].key = *key;
    s_state_cache[free_slot].value = value;
}

/* Значение canonical-свойства как число. false — свойства нет или значение невычислимо. */
static bool read_property_value(ha_entity_id_t entity, ha_property_id_t property, double *out)
{
    const ha_state_key_t key = {.entity = entity, .property = property};
    ha_state_record_t record = {0};
    if (sys_failed(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &key,
                                     &record))) {
        return false;
    }
    return semantics_value_to_double(&record.value, out);
}

/* Локальное время системного девайса из canonical-свойств ENTITY_SYSTEM. */
static bool read_system_time(uint16_t *minutes_of_day, uint8_t *weekday_mask)
{
    const ha_entity_id_t system = ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM);
    double minutes = 0.0;
    double mask = 0.0;
    if (!read_property_value(system, HA_PROPERTY_SYSTEM_MINUTES_OF_DAY, &minutes) ||
        !read_property_value(system, HA_PROPERTY_SYSTEM_WEEKDAY_MASK, &mask)) {
        return false;
    }
    *minutes_of_day = (uint16_t)minutes;
    *weekday_mask = (uint8_t)mask;
    return true;
}

static bool automation_collect(const void *key, const void *record, void *ctx)
{
    automation_scan_t *scan = (automation_scan_t *)ctx;
    const ha_rule_t *rule = (const ha_rule_t *)record;

    bool matched = false;
    if (scan->is_time) {
        matched = automation_rule_sem_time_matches(rule, scan->minutes_of_day, scan->weekday_mask);
    } else if (scan->is_state) {
        matched = automation_rule_sem_state_matches(rule, scan->source_entity, scan->state_entity,
                                                    scan->state_property, scan->state_value,
                                                    scan->state_prev_known, scan->state_prev_value);
    } else if (scan->is_event) {
        matched = automation_rule_sem_event_matches(rule, scan->source_entity, scan->event_id);
    }
    if (!matched) {
        return true;
    }

    if (scan->count >= AUTOMATION_FIRE_MAX) {
        ESP_LOGW(TAG, "more than %d matching rules; extra dropped", AUTOMATION_FIRE_MAX);
        return true;
    }
    scan->matches[scan->count].rule_id = ((const ha_automation_key_t *)key)->id;
    scan->matches[scan->count].rule = *rule;
    scan->count++;
    return true;
}

/* Все условия (AND) по canonical-свойствам. Нет значения — условие не выполнено. */
static bool automation_conditions_pass(ha_entity_id_t source_entity, const ha_rule_t *rule)
{
    const uint8_t count = (rule->conditions_count > HA_RULE_CONDITIONS_MAX)
                              ? HA_RULE_CONDITIONS_MAX
                              : rule->conditions_count;
    for (uint8_t i = 0; i < count; i++) {
        const ha_rule_condition_t *condition = &rule->conditions[i];
        const ha_entity_id_t entity =
            automation_rule_sem_resolve(condition->ref.entity, source_entity);
        double actual = 0.0;
        if (entity == HA_ENTITY_ID_NONE ||
            !read_property_value(entity, condition->ref.property, &actual)) {
            return false;
        }
        if (!automation_rule_sem_condition_ok(condition, actual)) {
            return false;
        }
    }
    return true;
}

static void automation_fire(const automation_match_t *match, ha_entity_id_t source_entity)
{
    if (!automation_conditions_pass(source_entity, &match->rule)) {
        ESP_LOGI(TAG, "rule %llu blocked by conditions", (unsigned long long)match->rule_id);
        return;
    }

    ha_property_command_t command = {0};
    if (!automation_rule_sem_action(&match->rule, source_entity, &command)) {
        ESP_LOGW(TAG, "rule %llu action not resolvable", (unsigned long long)match->rule_id);
        return;
    }

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_AUTOMATION;
    meta.value.type = (uint8_t)DOMAIN_VALUE_ENUM;
    meta.value.v.u32 = command.action;

    const sys_error_t err =
        domain_post(s_domain, HA_CMD_PROPERTY, &command, sizeof(command), NULL, &meta);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "rule %llu command not posted: entity=%llx err=%u",
                 (unsigned long long)match->rule_id, (unsigned long long)command.entity,
                 (unsigned)err.code);
        return;
    }
    ESP_LOGI(TAG, "rule %llu fired: entity=%llx property=%u action=%u",
             (unsigned long long)match->rule_id, (unsigned long long)command.entity,
             (unsigned)command.property, (unsigned)command.action);
}

static void automation_fire_matches(automation_scan_t *scan)
{
    for (size_t i = 0; i < scan->count; i++) {
        automation_fire(&scan->matches[i], scan->source_entity);
    }
}

static void automation_run_scan(automation_scan_t *scan)
{
    if (sys_failed(domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION,
                                      automation_collect, scan))) {
        return;
    }
    automation_fire_matches(scan);
}

static void automation_event_trigger(ha_entity_id_t source_entity, ha_event_id_t event_id)
{
    automation_scan_t scan = {.source_entity = source_entity, .is_event = true, .event_id = event_id};
    if (event_id == HA_EVENT_MINUTE_TICK) {
        scan.is_event = false;
        scan.is_time = read_system_time(&scan.minutes_of_day, &scan.weekday_mask);
        if (!scan.is_time) {
            return;
        }
    }
    automation_run_scan(&scan);
}

static void automation_state_trigger(const ha_state_key_t *key)
{
    ha_state_record_t record = {0};
    if (sys_failed(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, key,
                                     &record))) {
        return;
    }
    double value = 0.0;
    if (!semantics_value_to_double(&record.value, &value)) {
        return; /* значение невычислимо: триггерить нечем */
    }

    bool prev_known = false;
    double prev = 0.0;
    (void)state_cache_take(key, &prev_known, &prev);
    state_cache_put(key, value);

    automation_scan_t scan = {
        .source_entity = key->entity,
        .is_state = true,
        .state_entity = key->entity,
        .state_property = key->property,
        .state_value = value,
        .state_prev_known = prev_known,
        .state_prev_value = prev,
    };
    automation_run_scan(&scan);
}

static void automation_task(void *arg)
{
    (void)arg;

    for (;;) {
        domain_event_t event = {0};
        if (xQueueReceive(s_inbox, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.kind == (uint8_t)DOMAIN_FACT_EVENT) {
            /* A3.1: semantic event адресован источником-Entity (ключ = entity_id). */
            if (event.entity != (uint32_t)HA_ENTITY_ENTITY ||
                event.key_size != sizeof(ha_entity_id_t)) {
                continue;
            }
            ha_entity_id_t source_entity = HA_ENTITY_ID_NONE;
            memcpy(&source_entity, event.key, sizeof(source_entity));
            const ha_event_id_t event_id =
                (event.value.type == (uint8_t)DOMAIN_VALUE_ENUM) ? (ha_event_id_t)event.value.v.u32
                                                                 : HA_EVENT_NONE;
            automation_event_trigger(source_entity, event_id);
        } else if (event.kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED &&
                   event.entity == (uint32_t)HA_ENTITY_PROPERTY_STATE &&
                   event.key_size == sizeof(ha_state_key_t)) {
            ha_state_key_t key = {0};
            memcpy(&key, event.key, sizeof(key));
            automation_state_trigger(&key);
        }
    }
}

static sys_error_t subscribe(domain_t *domain, uint32_t kind_mask, domain_entity_t entity)
{
    domain_subscription_desc_t desc = {0};
    desc.kind_mask = kind_mask;
    desc.source_mask = (1u << (uint32_t)DOMAIN_SOURCE_ZIGBEE) |
                       (1u << (uint32_t)DOMAIN_SOURCE_SYSTEM);
    desc.entity = entity;
    desc.try_push = automation_accept;
    desc.wake = NULL; /* задача спит на очереди, отдельный сигнал не нужен */
    desc.ctx = NULL;

    domain_subscription_t *sub = NULL;
    return domain_subscribe(domain, &desc, &sub);
}

sys_error_t automation_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_INVALID_ARG);
    }
    s_domain = domain;
    memset(s_state_cache, 0, sizeof(s_state_cache));

    s_inbox = xQueueCreate(AUTOMATION_INBOX_LENGTH, sizeof(domain_event_t));
    if (s_inbox == NULL) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_NO_MEM);
    }

    /* Семантические события (entity-адресованные) — триггеры EVENT/TIME. */
    sys_error_t err = subscribe(domain, 1u << (uint32_t)DOMAIN_FACT_EVENT,
                                (domain_entity_t)HA_ENTITY_ENTITY);
    if (sys_failed(err)) {
        return err;
    }
    /* Canonical property state — триггер STATE и источники условий. */
    err = subscribe(domain, 1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED,
                    (domain_entity_t)HA_ENTITY_PROPERTY_STATE);
    if (sys_failed(err)) {
        return err;
    }

    if (xTaskCreate(automation_task, "automation", AUTOMATION_TASK_STACK, NULL,
                    AUTOMATION_TASK_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
