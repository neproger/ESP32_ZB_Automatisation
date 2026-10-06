#include "automation/automation.h"

#include <stddef.h>
#include <string.h>

#include "automation/automation_rule.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha_model/ha_automation.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_system.h"

/*
 * Задача сервиса — единственный, кто читает Domain и постит команды от лица Automation.
 * try_push только копирует факт в inbox (DOMAIN_API.md §9): мутирующий API там нельзя.
 *
 * Подписок две: EVENT (устройства) и ENTITY_UPSERTED (состояния) — для триггера STATE.
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

/*
 * Одному факту может соответствовать несколько правил. Собираем все совпадения в
 * обходе, а условия и постинг выполняем после: постить команду прямо в колбэке
 * обхода нельзя (там держится lock Domain).
 */
#define AUTOMATION_FIRE_MAX 8

typedef struct {
    uint64_t rule_id;
    ha_automation_record_t rule;
} automation_match_t;

typedef struct {
    /* DEVICE_EVENT/TIME */
    ha_device_uid_t device_uid;
    uint16_t command_id;
    bool has_time;
    uint16_t minutes_of_day;
    uint8_t weekday_mask;
    /* STATE */
    bool is_state;
    const ha_zb_state_key_t *state_key;
    double state_value;
    bool state_prev_known;
    double state_prev_value;

    automation_match_t matches[AUTOMATION_FIRE_MAX];
    size_t count;
} automation_scan_t;

/* Кэш последних значений состояний — нужен, чтобы отличать «стало истинно/ложно». */
#define AUTOMATION_STATE_CACHE 64

typedef struct {
    bool used;
    ha_zb_state_key_t key;
    double value;
} state_cache_slot_t;

static state_cache_slot_t s_state_cache[AUTOMATION_STATE_CACHE];

static bool state_cache_take(const ha_zb_state_key_t *key, bool *known, double *prev)
{
    for (size_t i = 0; i < AUTOMATION_STATE_CACHE; i++) {
        if (s_state_cache[i].used &&
            memcmp(&s_state_cache[i].key, key, sizeof(*key)) == 0) {
            *known = true;
            *prev = s_state_cache[i].value;
            return true;
        }
    }
    *known = false;
    return false; /* не найдено — не ошибка, просто первое наблюдение */
}

static void state_cache_put(const ha_zb_state_key_t *key, double value)
{
    size_t free_slot = AUTOMATION_STATE_CACHE;
    for (size_t i = 0; i < AUTOMATION_STATE_CACHE; i++) {
        if (s_state_cache[i].used &&
            memcmp(&s_state_cache[i].key, key, sizeof(*key)) == 0) {
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

/* Текущее локальное время системного девайса (минуты суток + маска дня недели). */
static bool read_system_time(uint16_t *minutes_of_day, uint8_t *weekday_mask)
{
    const ha_zb_state_key_t minutes_key = {.device_uid = HA_SYSTEM_DEVICE_UID,
                                           .cluster_id = HA_CLUSTER_SYSTEM,
                                           .attr_id = HA_SYS_ATTR_MINUTES_OF_DAY,
                                           .endpoint = HA_SYSTEM_ENDPOINT};
    ha_zb_state_record_t minutes_rec = {0};
    if (sys_failed(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_STATE, &minutes_key,
                                     &minutes_rec))) {
        return false;
    }

    const ha_zb_state_key_t mask_key = {.device_uid = HA_SYSTEM_DEVICE_UID,
                                        .cluster_id = HA_CLUSTER_SYSTEM,
                                        .attr_id = HA_SYS_ATTR_WEEKDAY_MASK,
                                        .endpoint = HA_SYSTEM_ENDPOINT};
    ha_zb_state_record_t mask_rec = {0};
    if (sys_failed(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_STATE, &mask_key,
                                     &mask_rec))) {
        return false;
    }

    *minutes_of_day = (uint16_t)minutes_rec.raw;
    *weekday_mask = (uint8_t)mask_rec.raw;
    return true;
}

static bool automation_collect(const void *key, const void *record, void *ctx)
{
    automation_scan_t *scan = (automation_scan_t *)ctx;
    const ha_automation_record_t *rule = (const ha_automation_record_t *)record;

    switch (rule->trigger_kind) {
    case (uint8_t)HA_TRIGGER_TIME:
        if (!scan->has_time ||
            !automation_rule_time_matches(rule, scan->minutes_of_day, scan->weekday_mask)) {
            return true;
        }
        break;
    case (uint8_t)HA_TRIGGER_STATE:
        if (!scan->is_state ||
            !automation_rule_state_matches(rule, scan->state_key, scan->state_value,
                                           scan->state_prev_known, scan->state_prev_value)) {
            return true;
        }
        break;
    default: /* DEVICE_EVENT */
        if (!automation_rule_matches(rule, scan->device_uid, scan->command_id)) {
            return true;
        }
        break;
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

/*
 * Условия правила — AND: все должны быть выполнены. Нет состояния по ключу или тип
 * значения невычислим — условие не выполнено (как в правилах v1).
 */
static bool automation_conditions_pass(ha_device_uid_t trigger_uid,
                                       const ha_automation_record_t *rule)
{
    const uint8_t count = (rule->conditions_count > HA_AUTOMATION_CONDITIONS_MAX)
                              ? HA_AUTOMATION_CONDITIONS_MAX
                              : rule->conditions_count;
    for (uint8_t i = 0; i < count; i++) {
        const ha_automation_condition_t *condition = &rule->conditions[i];

        ha_zb_state_key_t key = {0};
        key.device_uid = (condition->device_uid != 0) ? condition->device_uid : trigger_uid;
        key.cluster_id = condition->cluster_id;
        key.attr_id = condition->attr_id;
        key.endpoint = condition->endpoint;

        ha_zb_state_record_t state = {0};
        if (sys_failed(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_STATE, &key, &state))) {
            return false;
        }
        if (!automation_rule_condition_ok(condition, &state)) {
            return false;
        }
    }
    return true;
}

static void automation_fire(const automation_match_t *match, ha_device_uid_t trigger_uid)
{
    if (!automation_conditions_pass(trigger_uid, &match->rule)) {
        ESP_LOGI(TAG, "rule %llu blocked by conditions", (unsigned long long)match->rule_id);
        return;
    }

    ha_zb_command_t command = {0};
    if (sys_failed(automation_rule_command(&match->rule, trigger_uid, &command))) {
        return;
    }

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_AUTOMATION;
    meta.value.type = (uint8_t)DOMAIN_VALUE_ENUM;
    meta.value.v.u32 = command.command_id;

    const sys_error_t err = domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command),
                                        &target, &meta);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "command not posted: uid=%llx err=%u",
                 (unsigned long long)command.device_uid, (unsigned)err.code);
        return;
    }
    ESP_LOGI(TAG, "rule %llu fired: uid=%llx cluster=%04x cmd=%02x",
             (unsigned long long)match->rule_id, (unsigned long long)command.device_uid,
             (unsigned)command.cluster_id, (unsigned)command.command_id);
}

static void automation_fire_matches(automation_scan_t *scan, ha_device_uid_t trigger_uid)
{
    for (size_t i = 0; i < scan->count; i++) {
        automation_fire(&scan->matches[i], trigger_uid);
    }
}

static void automation_event_trigger(ha_device_uid_t device_uid, uint16_t command_id)
{
    automation_scan_t scan = {.device_uid = device_uid, .command_id = command_id};
    /* «Будильники» проверяются на минутном тике системного девайса. */
    if (device_uid == HA_SYSTEM_DEVICE_UID &&
        command_id == (uint16_t)HA_SYS_EVENT_MINUTE_TICK) {
        scan.has_time = read_system_time(&scan.minutes_of_day, &scan.weekday_mask);
    }
    if (sys_failed(domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION,
                                      automation_collect, &scan))) {
        return;
    }
    automation_fire_matches(&scan, device_uid);
}

static void automation_state_trigger(const ha_zb_state_key_t *key)
{
    ha_zb_state_record_t record = {0};
    if (sys_failed(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_STATE, key, &record))) {
        return;
    }
    double value = 0.0;
    if (!automation_rule_state_value(&record, &value)) {
        return; /* тип вне словаря скаляров: триггерить нечем */
    }

    bool prev_known = false;
    double prev = 0.0;
    (void)state_cache_take(key, &prev_known, &prev);
    state_cache_put(key, value);

    automation_scan_t scan = {
        .is_state = true,
        .state_key = key,
        .state_value = value,
        .state_prev_known = prev_known,
        .state_prev_value = prev,
    };
    if (sys_failed(domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION,
                                      automation_collect, &scan))) {
        return;
    }
    automation_fire_matches(&scan, key->device_uid);
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
            if (event.key_size != sizeof(ha_device_uid_t)) {
                continue;
            }
            ha_device_uid_t device_uid = 0;
            memcpy(&device_uid, event.key, sizeof(device_uid));
            const uint16_t command_id =
                (event.value.type == (uint8_t)DOMAIN_VALUE_ENUM) ? (uint16_t)event.value.v.u32 : 0;
            automation_event_trigger(device_uid, command_id);
        } else if (event.kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED &&
                   event.entity == (uint32_t)HA_ENTITY_STATE &&
                   event.key_size == sizeof(ha_zb_state_key_t)) {
            ha_zb_state_key_t key = {0};
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

    /* События устройств (EVENT) — триггеры DEVICE_EVENT/TIME. */
    sys_error_t err = subscribe(domain, 1u << (uint32_t)DOMAIN_FACT_EVENT,
                                (domain_entity_t)HA_ENTITY_DEVICE);
    if (sys_failed(err)) {
        return err;
    }
    /* Изменения состояний (ENTITY_UPSERTED) — триггер STATE. */
    err = subscribe(domain, 1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED,
                    (domain_entity_t)HA_ENTITY_STATE);
    if (sys_failed(err)) {
        return err;
    }

    if (xTaskCreate(automation_task, "automation", AUTOMATION_TASK_STACK, NULL,
                    AUTOMATION_TASK_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
