#include "zigbee/zigbee_diag.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/*
 * Счётчики — общие на сервис: сбой может прийти из задачи сервиса, из executor'а
 * (контекст вызывающего) и из источника кадров. Обновление идёт в критической секции:
 * это пара инструкций, отдельный мьютекс ради них не нужен.
 */

static const char *TAG = "zigbee.diag";

static zigbee_diag_counters_t s_counters;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *kind_name(zigbee_diag_kind_t kind)
{
    switch (kind) {
    case ZIGBEE_DIAG_REPORT: return "report";
    case ZIGBEE_DIAG_REPORT_QUEUE: return "report queue full";
    case ZIGBEE_DIAG_COMMAND: return "command rejected";
    case ZIGBEE_DIAG_SEND: return "command not sent";
    case ZIGBEE_DIAG_TOPOLOGY: return "topology";
    case ZIGBEE_DIAG_EVENT: return "event";
    default: return "unknown";
    }
}

void zigbee_diag_record(zigbee_diag_kind_t kind, sys_error_t err)
{
    if (kind >= ZIGBEE_DIAG_KIND_COUNT) {
        return;
    }

    uint32_t total = 0;
    portENTER_CRITICAL(&s_lock);
    zigbee_diag_entry_t *entry = &s_counters.entries[kind];
    entry->total++;
    entry->last_layer = err.layer;
    entry->last_code = err.code;
    total = entry->total;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGW(TAG, "%s: layer=%u code=%u (total=%lu)", kind_name(kind), (unsigned)err.layer,
             (unsigned)err.code, (unsigned long)total);
}

void zigbee_diag_snapshot(zigbee_diag_counters_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    *out = s_counters;
    portEXIT_CRITICAL(&s_lock);
}
