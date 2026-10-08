#include "journal_console.h"

#include <stdio.h>
#include <string.h>

#include "domain/domain_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha_model/ha_entities.h"

static const char *TAG = "journal";

/*
 * Консольный журнал — обычный подписчик со своим inbox и задачей
 * (docs/domain/DISPATCHER.md §1): try_push только кладёт факт в очередь, печать идёт в
 * отдельной задаче. Иначе блокирующая запись в UART (115200, ~8 мс/строка) выполнялась
 * бы в задаче Dispatcher'а под dispatch_lock и задерживала доставку остальным
 * подписчикам (Web, Automation) — их отставание росло бы с частотой событий.
 */
#define JOURNAL_CONSOLE_QUEUE 16
#define JOURNAL_CONSOLE_STACK 3584
#define JOURNAL_CONSOLE_PRIORITY 1

static QueueHandle_t s_inbox;

static const char *fact_kind_name(uint8_t kind)
{
    switch (kind) {
    case DOMAIN_FACT_ENTITY_UPSERTED: return "ENTITY_UPSERTED";
    case DOMAIN_FACT_ENTITY_REMOVED: return "ENTITY_REMOVED";
    case DOMAIN_FACT_EVENT: return "EVENT";
    case DOMAIN_FACT_COMMAND_SENT: return "COMMAND_SENT";
    case DOMAIN_FACT_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static const char *fact_op_name(uint8_t op)
{
    switch (op) {
    case DOMAIN_OP_ENTITY_PUT: return "ENTITY_PUT";
    case DOMAIN_OP_ENTITY_REMOVE: return "ENTITY_REMOVE";
    case DOMAIN_OP_COMMAND: return "COMMAND";
    case DOMAIN_OP_PAYLOAD_PUT: return "PAYLOAD_PUT";
    case DOMAIN_OP_ENTITY_GET: return "ENTITY_GET";
    case DOMAIN_OP_ENTITY_ITER: return "ENTITY_ITER";
    default: return "UNKNOWN";
    }
}

static const char *fact_source_name(uint8_t source)
{
    switch (source) {
    case DOMAIN_SOURCE_ZIGBEE: return "ZIGBEE";
    case DOMAIN_SOURCE_UI: return "UI";
    case DOMAIN_SOURCE_AUTOMATION: return "AUTOMATION";
    case DOMAIN_SOURCE_SYSTEM: return "SYSTEM";
    default: return "UNKNOWN";
    }
}

static void value_text(const domain_value_t *value, char *out, size_t size)
{
    switch (value->type) {
    case DOMAIN_VALUE_BOOL: snprintf(out, size, "bool=%u", (unsigned)value->v.u32); break;
    case DOMAIN_VALUE_I32: snprintf(out, size, "i32=%d", (int)value->v.i32); break;
    case DOMAIN_VALUE_U32: snprintf(out, size, "u32=%lu", (unsigned long)value->v.u32); break;
    case DOMAIN_VALUE_F32: snprintf(out, size, "f32=%.3f", (double)value->v.f32); break;
    case DOMAIN_VALUE_ENUM: snprintf(out, size, "enum=%lu", (unsigned long)value->v.u32); break;
    default: snprintf(out, size, "value=-"); break;
    }
}

/*
 * Ключи имеют общий layout сервисов (ha_model), поэтому печатаются по полям. Тип
 * сущности нужен: ключ состояния и ключ endpoint'а одной ширины, по размеру их не
 * различить.
 */
static void format_key(uint32_t entity, const uint8_t *key, uint8_t key_size, char *out,
                       size_t size)
{
    if (entity == (uint32_t)HA_ENTITY_STATE && key_size == sizeof(ha_zb_state_key_t)) {
        const ha_zb_state_key_t *state_key = (const ha_zb_state_key_t *)key;
        snprintf(out, size, "uid=%llx ep=%u cluster=%04x attr=%04x",
                 (unsigned long long)state_key->device_uid, (unsigned)state_key->endpoint,
                 (unsigned)state_key->cluster_id, (unsigned)state_key->attr_id);
        return;
    }

    if (entity == (uint32_t)HA_ENTITY_ENDPOINT && key_size == sizeof(ha_endpoint_key_t)) {
        const ha_endpoint_key_t *endpoint_key = (const ha_endpoint_key_t *)key;
        snprintf(out, size, "uid=%llx ep=%u", (unsigned long long)endpoint_key->device_uid,
                 (unsigned)endpoint_key->endpoint);
        return;
    }

    if (key_size == sizeof(ha_device_uid_t)) {
        ha_device_uid_t uid = 0;
        memcpy(&uid, key, sizeof(uid));
        snprintf(out, size, "uid=%llx", (unsigned long long)uid);
        return;
    }

    for (uint8_t i = 0; i < key_size && i * 3 + 1 < size; i++) {
        snprintf(out + i * 3, 4, "%02x ", key[i]);
    }
}

static bool journal_log(const domain_event_t *event, void *ctx)
{
    (void)ctx;

    char key_text[72] = {0};
    format_key(event->entity, event->key, event->key_size, key_text, sizeof(key_text));

    char value[24] = {0};
    value_text(&event->value, value, sizeof(value));

    char tail[48] = {0};
    if (event->kind == (uint8_t)DOMAIN_FACT_ERROR) {
        snprintf(tail, sizeof(tail), "err layer=%u code=%u", (unsigned)event->error.layer,
                 (unsigned)event->error.code);
    } else if (event->payload_ref != 0) {
        snprintf(tail, sizeof(tail), "payload=%llu size=%u",
                 (unsigned long long)event->payload_ref, (unsigned)event->payload_size);
    }

    ESP_LOGI(TAG, "#%llu ts=%llums %s op=%s src=%s entity=%lu key=[%s] %s %s",
             (unsigned long long)event->event_id, (unsigned long long)event->ts,
             fact_kind_name(event->kind), fact_op_name(event->op), fact_source_name(event->source),
             (unsigned long)event->entity, key_text, value, tail);
    return true;
}

static bool journal_console_try_push(const domain_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_inbox == NULL) {
        return false; /* очередь полна/нет — это локальная потеря диагностики */
    }
    return xQueueSend(s_inbox, event, 0) == pdTRUE;
}

static void journal_console_task(void *arg)
{
    (void)arg;
    for (;;) {
        domain_event_t event = {0};
        if (xQueueReceive(s_inbox, &event, portMAX_DELAY) == pdTRUE) {
            journal_log(&event, NULL);
        }
    }
}

sys_error_t journal_console_subscribe(domain_t *domain)
{
    if (s_inbox == NULL) {
        s_inbox = xQueueCreate(JOURNAL_CONSOLE_QUEUE, sizeof(domain_event_t));
        if (s_inbox == NULL) {
            return sys_error_make(SYS_LAYER_DOMAIN, SYS_CODE_NO_MEM);
        }
    }
    if (xTaskCreate(journal_console_task, "journal", JOURNAL_CONSOLE_STACK, NULL,
                    JOURNAL_CONSOLE_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_DOMAIN, SYS_CODE_NO_MEM);
    }

    domain_subscription_desc_t desc = {0};
    desc.kind_mask = 0; /* любые факты */
    desc.source_mask = 0;
    desc.entity = 0;
    desc.try_push = journal_console_try_push;
    desc.wake = NULL;
    desc.ctx = NULL;

    domain_subscription_t *sub = NULL;
    return domain_subscribe(domain, &desc, &sub);
}
