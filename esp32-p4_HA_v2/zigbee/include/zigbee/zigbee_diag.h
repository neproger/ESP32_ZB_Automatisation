#pragma once

#include "sys/sys_error.h"

/*
 * Диагностика сервиса — отдельный поток ошибок, а не Journal
 * (docs/services/ZIGBEE.md §7, docs/domain/JOURNAL.md:124-127).
 *
 * Причина разделения: ERROR-факт в Journal — исход конкретной операции, и пачка
 * однотипных сбоев вытесняет из ring факты состояния. Устойчивая картина «что и как
 * часто ломается» живёт здесь: счётчики по видам сбоя и последняя ошибка.
 */

typedef enum {
    ZIGBEE_DIAG_REPORT = 0,  /* репорт не стал состоянием */
    ZIGBEE_DIAG_REPORT_QUEUE, /* очередь репортов заполнена */
    ZIGBEE_DIAG_COMMAND,      /* команда отклонена до отправки */
    ZIGBEE_DIAG_SEND,         /* команда не ушла в радио */
    ZIGBEE_DIAG_TOPOLOGY,     /* топология не записана */
    ZIGBEE_DIAG_EVENT,        /* событие не опубликовано */
    ZIGBEE_DIAG_KIND_COUNT
} zigbee_diag_kind_t;

typedef struct {
    uint32_t total;
    uint16_t last_layer;
    uint16_t last_code;
} zigbee_diag_entry_t;

typedef struct {
    zigbee_diag_entry_t entries[ZIGBEE_DIAG_KIND_COUNT];
} zigbee_diag_counters_t;

/* Записать сбой и напечатать его в консоль своим тегом. */
void zigbee_diag_record(zigbee_diag_kind_t kind, sys_error_t err);

/* Снимок счётчиков: copy-out, как и любое чтение состояния в проекте. */
void zigbee_diag_snapshot(zigbee_diag_counters_t *out);
