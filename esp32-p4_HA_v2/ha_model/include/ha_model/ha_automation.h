#pragma once

/*
 * Форма правила автоматизации (docs/services/AUTOMATION.md). Словарь, а не логика:
 * только поля и лимиты. Живёт как entity `automation` в Domain; Automation service
 * читает правило, сопоставляет с фактом и постит команду.
 *
 * Правило минимально: триггер — событие от устройства (device + command), действие —
 * Zigbee-команда. `action_device_uid == 0` означает «то же устройство, что вызвало».
 */

#include <stdint.h>

#include "ha_model/ha_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA_AUTOMATION_ARGS_MAX 8

/* Ключ правила — числовой id: правило адресуется независимо от устройства. */
typedef struct {
    uint64_t id;
} ha_automation_key_t;

typedef struct {
    uint8_t enabled;
    uint8_t action_args_len;
    uint8_t reserved[6];

    /* Триггер: событие (EVENT) от устройства. 0 — «любое». */
    ha_device_uid_t trigger_device_uid;
    uint16_t trigger_command_id;

    /* Действие: Zigbee-команда. device_uid == 0 — устройство-источник события. */
    ha_device_uid_t action_device_uid;
    uint8_t action_endpoint;
    uint16_t action_cluster_id;
    uint8_t action_command_id;
    uint8_t action_args[HA_AUTOMATION_ARGS_MAX];
} ha_automation_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
static_assert(sizeof(ha_automation_record_t) <= 1024, "automation: больше региона");
#else
_Static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
_Static_assert(sizeof(ha_automation_record_t) <= 1024, "automation: больше региона");
#endif

#ifdef __cplusplus
}
#endif
