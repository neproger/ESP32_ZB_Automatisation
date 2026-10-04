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

/*
 * Задача сервиса — единственный, кто читает Domain и постит команды от лица Automation.
 * try_push только копирует факт в inbox (DOMAIN_API.md §9): мутирующий API там нельзя.
 */

#define AUTOMATION_INBOX_LENGTH 16
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
 * Одному событию (device, command) может соответствовать несколько правил. Собираем
 * все совпадения в обходе, а условия и постинг выполняем после: постить команду прямо
 * в колбэке обхода нельзя (там держится lock Domain).
 */
#define AUTOMATION_FIRE_MAX 8

typedef struct {
    uint64_t rule_id;
    ha_automation_record_t rule;
} automation_match_t;

typedef struct {
    ha_device_uid_t device_uid;
    uint16_t command_id;
    automation_match_t matches[AUTOMATION_FIRE_MAX];
    size_t count;
} automation_scan_t;

static bool automation_collect(const void *key, const void *record, void *ctx)
{
    automation_scan_t *scan = (automation_scan_t *)ctx;
    const ha_automation_record_t *rule = (const ha_automation_record_t *)record;
    if (!automation_rule_matches(rule, scan->device_uid, scan->command_id)) {
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

static void automation_trigger(ha_device_uid_t device_uid, uint16_t command_id)
{
    automation_scan_t scan = {.device_uid = device_uid, .command_id = command_id};
    if (sys_failed(domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION,
                                      automation_collect, &scan))) {
        return;
    }
    for (size_t i = 0; i < scan.count; i++) {
        automation_fire(&scan.matches[i], device_uid);
    }
}

static void automation_task(void *arg)
{
    (void)arg;

    for (;;) {
        domain_event_t event = {0};
        if (xQueueReceive(s_inbox, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.kind != (uint8_t)DOMAIN_FACT_EVENT || event.key_size != sizeof(ha_device_uid_t)) {
            continue;
        }

        ha_device_uid_t device_uid = 0;
        memcpy(&device_uid, event.key, sizeof(device_uid));
        const uint16_t command_id =
            (event.value.type == (uint8_t)DOMAIN_VALUE_ENUM) ? (uint16_t)event.value.v.u32 : 0;

        automation_trigger(device_uid, command_id);
    }
}

sys_error_t automation_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_INVALID_ARG);
    }
    s_domain = domain;

    s_inbox = xQueueCreate(AUTOMATION_INBOX_LENGTH, sizeof(domain_event_t));
    if (s_inbox == NULL) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_NO_MEM);
    }

    domain_subscription_desc_t desc = {0};
    desc.kind_mask = 1u << (uint32_t)DOMAIN_FACT_EVENT;
    /* Триггеры бывают и от синтетического системного устройства (время/погода). */
    desc.source_mask = (1u << (uint32_t)DOMAIN_SOURCE_ZIGBEE) |
                       (1u << (uint32_t)DOMAIN_SOURCE_SYSTEM);
    desc.entity = (domain_entity_t)HA_ENTITY_DEVICE;
    desc.try_push = automation_accept;
    desc.wake = NULL; /* задача спит на очереди, отдельный сигнал не нужен */
    desc.ctx = NULL;

    domain_subscription_t *sub = NULL;
    const sys_error_t subscribed = domain_subscribe(domain, &desc, &sub);
    if (sys_failed(subscribed)) {
        return subscribed;
    }

    if (xTaskCreate(automation_task, "automation", AUTOMATION_TASK_STACK, NULL,
                    AUTOMATION_TASK_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_AUTOMATION, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
