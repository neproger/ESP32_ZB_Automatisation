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

/*
 * Задача сервиса — единственный, кто читает Domain и постит команды от лица Automation.
 * try_push только копирует факт в inbox (DOMAIN_API.md §9): мутирующий API там нельзя.
 *
 * A3: правила хранятся как ha_rule_t (entity-адресованные). Триггеры: EVENT (машина
 * событий HA_ENTITY_ENTITY), STATE (canonical HA_ENTITY_PROPERTY_STATE), TIME (canonical
 * SYSTEM-свойства). Действие — semantic-команда HA_CMD_PROPERTY.
 *
 * Runtime Index (производный, RAM-only): компактный список ключей триггеров, чтобы на
 * каждый факт не перечитывать все правила. Source of truth — только HA_ENTITY_AUTOMATION;
 * индекс не persistится и всегда восстанавливается из правил (rebuild на изменение).
 */

#define AUTOMATION_INBOX_LENGTH 32
#define AUTOMATION_TASK_STACK 4096
#define AUTOMATION_TASK_PRIORITY 5

static const char *TAG = "automation";

static domain_t *s_domain;
static QueueHandle_t s_inbox;

/* --- runtime index ------------------------------------------------------- */

#define AUTOMATION_INDEX_MAX 128

typedef struct {
    uint64_t rule_id;
    uint8_t trigger_kind; /* ha_automation_trigger_kind_t */
    ha_entity_id_t entity;  /* EVENT/STATE; HA_ENTITY_ID_SOURCE = источник */
    ha_property_id_t property;
    ha_event_id_t event_id;
} automation_index_entry_t;

static automation_index_entry_t s_index[AUTOMATION_INDEX_MAX];
static size_t s_index_count;

static bool automation_index_add(const void *key, const void *record, void *ctx)
{
    (void)ctx;
    const ha_rule_t *rule = (const ha_rule_t *)record;
    if (rule->enabled == 0) {
        return true; /* выключенные правила в индексе не нужны */
    }
    if (s_index_count >= AUTOMATION_INDEX_MAX) {
        ESP_LOGW(TAG, "index full (%d); extra rules not indexed", AUTOMATION_INDEX_MAX);
        return true;
    }
    automation_index_entry_t *entry = &s_index[s_index_count++];
    entry->rule_id = ((const ha_automation_key_t *)key)->id;
    entry->trigger_kind = rule->trigger.kind;
    entry->entity = HA_ENTITY_ID_NONE;
    entry->property = HA_PROPERTY_UNKNOWN;
    entry->event_id = HA_EVENT_NONE;
    if (rule->trigger.kind == (uint8_t)HA_TRIGGER_DEVICE_EVENT ||
        rule->trigger.kind == (uint8_t)HA_TRIGGER_STATE) {
        entry->entity = rule->trigger.entity;
    }
    if (rule->trigger.kind == (uint8_t)HA_TRIGGER_STATE) {
        entry->property = rule->trigger.property;
    }
    if (rule->trigger.kind == (uint8_t)HA_TRIGGER_DEVICE_EVENT) {
        entry->event_id = rule->trigger.event_id;
    }
    return true;
}

/* Полная перестройка из правил. Вызывается на старте и на любое изменение правила. */
static void automation_index_rebuild(void)
{
    s_index_count = 0;
    if (sys_failed(domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION,
                                      automation_index_add, NULL))) {
        s_index_count = 0; /* индекс не строится — совпадений не будет (persisted data цел) */
    }
    ESP_LOGD(TAG, "runtime index rebuilt: %u entries", (unsigned)s_index_count);
}

/* --- правило и действие -------------------------------------------------- */

static bool automation_rule_get(uint64_t rule_id, ha_rule_t *out)
{
    const ha_automation_key_t key = {.id = rule_id};
    return sys_ok(
        domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, &key, out));
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
    return ha_value_to_double(&record.value, out);
}

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

static void automation_fire(uint64_t rule_id, ha_entity_id_t source_entity)
{
    ha_rule_t rule = {0};
    if (!automation_rule_get(rule_id, &rule)) {
        return; /* правило исчезло между индексом и сейчас */
    }
    if (!automation_conditions_pass(source_entity, &rule)) {
        ESP_LOGI(TAG, "rule %llu blocked by conditions", (unsigned long long)rule_id);
        return;
    }
    ha_property_command_t command = {0};
    if (!automation_rule_sem_action(&rule, source_entity, &command)) {
        ESP_LOGW(TAG, "rule %llu action not resolvable", (unsigned long long)rule_id);
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
                 (unsigned long long)rule_id, (unsigned long long)command.entity,
                 (unsigned)err.code);
        return;
    }
    ESP_LOGI(TAG, "rule %llu fired: entity=%llx property=%u action=%u",
             (unsigned long long)rule_id, (unsigned long long)command.entity,
             (unsigned)command.property, (unsigned)command.action);
}

/* --- кэш предыдущих значений (для edge) ---------------------------------- */

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
    return false;
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
        free_slot = 0;
    }
    s_state_cache[free_slot].used = true;
    s_state_cache[free_slot].key = *key;
    s_state_cache[free_slot].value = value;
}

/* --- триггеры ------------------------------------------------------------ */

/* Совпадение адреса триггера (в индексе) с фактическим: SOURCE подходит любому. */
static bool index_address_matches(ha_entity_id_t index_entity, ha_entity_id_t actual)
{
    return index_entity == HA_ENTITY_ID_SOURCE || index_entity == actual;
}

static void automation_event_trigger(ha_entity_id_t source_entity, ha_event_id_t event_id)
{
    if (event_id == HA_EVENT_MINUTE_TICK) {
        uint16_t minutes = 0;
        uint8_t mask = 0;
        if (!read_system_time(&minutes, &mask)) {
            return;
        }
        for (size_t i = 0; i < s_index_count; i++) {
            if (s_index[i].trigger_kind != (uint8_t)HA_TRIGGER_TIME) {
                continue;
            }
            ha_rule_t rule = {0};
            if (!automation_rule_get(s_index[i].rule_id, &rule)) {
                continue;
            }
            if (automation_rule_sem_time_matches(&rule, minutes, mask)) {
                automation_fire(s_index[i].rule_id, source_entity);
            }
        }
        return;
    }

    for (size_t i = 0; i < s_index_count; i++) {
        if (s_index[i].trigger_kind != (uint8_t)HA_TRIGGER_DEVICE_EVENT ||
            s_index[i].event_id != event_id ||
            !index_address_matches(s_index[i].entity, source_entity)) {
            continue;
        }
        automation_fire(s_index[i].rule_id, source_entity);
    }
}

static void automation_state_trigger(const ha_state_key_t *key)
{
    ha_state_record_t record = {0};
    if (sys_failed(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, key,
                                     &record))) {
        return;
    }
    double value = 0.0;
    if (!ha_value_to_double(&record.value, &value)) {
        return;
    }

    bool prev_known = false;
    double prev = 0.0;
    (void)state_cache_take(key, &prev_known, &prev);
    state_cache_put(key, value);

    for (size_t i = 0; i < s_index_count; i++) {
        if (s_index[i].trigger_kind != (uint8_t)HA_TRIGGER_STATE ||
            s_index[i].property != key->property ||
            !index_address_matches(s_index[i].entity, key->entity)) {
            continue;
        }
        ha_rule_t rule = {0};
        if (!automation_rule_get(s_index[i].rule_id, &rule)) {
            continue;
        }
        if (automation_rule_sem_state_matches(&rule, key->entity, key->entity, key->property, value,
                                              prev_known, prev)) {
            /*
             * Loop-guard: STATE-правило, чьё действие адресует то же (entity, property), что и
             * триггер, зациклилось бы (команда → репорт → снова триггер). Пропускаем.
             */
            const ha_entity_id_t action_entity =
                (rule.action_ref.entity == HA_ENTITY_ID_SOURCE) ? key->entity
                                                                : rule.action_ref.entity;
            if (action_entity == key->entity && rule.action_ref.property == key->property) {
                ESP_LOGW(TAG, "rule %llu self-trigger skipped: entity=%llx property=%u",
                         (unsigned long long)s_index[i].rule_id, (unsigned long long)key->entity,
                         (unsigned)key->property);
                continue;
            }
            automation_fire(s_index[i].rule_id, key->entity);
        }
    }
}

/* --- задача -------------------------------------------------------------- */

static bool automation_accept(const domain_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_inbox == NULL) {
        return false;
    }
    return xQueueSend(s_inbox, event, 0) == pdTRUE;
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
            if (event.entity != (uint32_t)HA_ENTITY_ENTITY ||
                event.key_size != sizeof(ha_entity_id_t)) {
                continue;
            }
            ha_event_t ev = {0};
            if (event.payload_ref == 0 || event.payload_size != sizeof(ev) ||
                sys_failed(domain_payload_get(s_domain, event.payload_ref, &ev, sizeof(ev)))) {
                continue; /* payload вытеснен — событие пропущено */
            }
            automation_event_trigger(ev.source_entity, ev.id);
        } else if (event.kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED &&
                   event.entity == (uint32_t)HA_ENTITY_PROPERTY_STATE &&
                   event.key_size == sizeof(ha_state_key_t)) {
            ha_state_key_t key = {0};
            memcpy(&key, event.key, sizeof(key));
            automation_state_trigger(&key);
        } else if (event.entity == (uint32_t)HA_ENTITY_AUTOMATION &&
                   (event.kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED ||
                    event.kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED)) {
            /* Правило создано/изменено/удалено — перестраиваем производный индекс. */
            automation_index_rebuild();
        }
    }
}

static sys_error_t subscribe(domain_t *domain, uint32_t kind_mask, uint32_t source_mask,
                             domain_entity_t entity)
{
    domain_subscription_desc_t desc = {0};
    desc.kind_mask = kind_mask;
    desc.source_mask = source_mask;
    desc.entity = entity;
    desc.try_push = automation_accept;
    desc.wake = NULL;
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

    /*
     * Фильтр по canonical типу факта, а не по adapter origin (source_mask 0 = любой):
     * Automation не знает, какой transport породил Entity/событие.
     */

    /* Семантические события (entity-адресованные) — триггеры EVENT/TIME. */
    sys_error_t err = subscribe(domain, 1u << (uint32_t)DOMAIN_FACT_EVENT, 0u,
                                (domain_entity_t)HA_ENTITY_ENTITY);
    if (sys_failed(err)) {
        return err;
    }
    /* Canonical property state — триггер STATE и источники условий. */
    err = subscribe(domain, 1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED, 0u,
                    (domain_entity_t)HA_ENTITY_PROPERTY_STATE);
    if (sys_failed(err)) {
        return err;
    }
    /* Изменения правил (создаёт UI) — перестройка индекса. Любой источник. */
    err = subscribe(domain,
                    1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED |
                        1u << (uint32_t)DOMAIN_FACT_ENTITY_REMOVED,
                    0u, (domain_entity_t)HA_ENTITY_AUTOMATION);
    if (sys_failed(err)) {
        return err;
    }

    automation_index_rebuild();

    if (xTaskCreate(automation_task, "automation", AUTOMATION_TASK_STACK, NULL,
                    AUTOMATION_TASK_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
