#pragma once

/*
 * Форма правила автоматизации (docs/services/AUTOMATION.md). Словарь, а не логика:
 * только поля и лимиты. Живёт как entity `automation` в Domain; Automation service
 * читает правило, сопоставляет с фактом и постит команду.
 *
 * Правило: триггер — событие от устройства (device + command), действие —
 * Zigbee-команда, условия — сравнение атрибутов состояния. `device_uid == 0`
 * (в действии и в условии) означает «то же устройство, что вызвало».
 *
 * Условия соединяются по И (AND): правило срабатывает, только если выполнены все.
 * Значение сравнивается числом: ZCL-значение состояния декодируется по `zcl_type`,
 * а `value` условия задаётся числом (bool как 0/1).
 */

#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA_AUTOMATION_ARGS_MAX 8
#define HA_AUTOMATION_CONDITIONS_MAX 4

/* Ключ правила — числовой id: правило адресуется независимо от устройства. */
typedef struct {
    uint64_t id;
} ha_automation_key_t;

/*
 * Вид триггера. DEVICE_EVENT — событие от устройства (Zigbee-команда или системное
 * событие); TIME — «будильник»: конкретное время суток + набор дней недели, без
 * условий сравнения. Расширяемо: сюда добавятся STATE (порог атрибута) и SUN.
 */
typedef enum {
    HA_TRIGGER_DEVICE_EVENT = 0,
    HA_TRIGGER_TIME = 1,
} ha_automation_trigger_kind_t;

/* Оператор условия: те же шесть, что в правилах v1 (docs/services/AUTOMATION.md). */
typedef enum {
    HA_CONDITION_OP_EQ = 1,
    HA_CONDITION_OP_NE = 2,
    HA_CONDITION_OP_GT = 3,
    HA_CONDITION_OP_LT = 4,
    HA_CONDITION_OP_GE = 5,
    HA_CONDITION_OP_LE = 6,
    /* Битовая маска: (value & actual) != 0. Для наборов, где EQ/NE мало (дни недели). */
    HA_CONDITION_OP_HAS_BITS = 7,
} ha_condition_op_t;

/*
 * Условие — тот же числовой ключ состояния, что и в Entity Store (cluster/attr),
 * плюс оператор и порог. `device_uid == 0` — устройство-источник триггера,
 * `endpoint == 0` — любой endpoint.
 */
typedef struct {
    ha_device_uid_t device_uid;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t endpoint;
    uint8_t op; /* ha_condition_op_t */
    uint8_t reserved[2];
    float value;
} ha_automation_condition_t;

typedef struct {
    uint8_t enabled;
    uint8_t action_args_len;
    uint8_t conditions_count;
    uint8_t trigger_kind;             /* ha_automation_trigger_kind_t */

    /* TIME: время срабатывания (минуты от начала суток) и дни недели (бит 0=Пн..6=Вс). */
    uint16_t trigger_minutes_of_day;
    uint8_t trigger_weekday_mask;
    uint8_t reserved;

    /* DEVICE_EVENT: событие (EVENT) от устройства. 0 — «любое». */
    ha_device_uid_t trigger_device_uid;
    uint16_t trigger_command_id;

    /* Действие: Zigbee-команда. device_uid == 0 — устройство-источник события. */
    ha_device_uid_t action_device_uid;
    uint8_t action_endpoint;
    uint16_t action_cluster_id;
    uint8_t action_command_id;
    uint8_t action_args[HA_AUTOMATION_ARGS_MAX];

    /* Условия (AND); первые conditions_count значимы. */
    ha_automation_condition_t conditions[HA_AUTOMATION_CONDITIONS_MAX];
} ha_automation_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
static_assert(sizeof(ha_automation_condition_t) == 24, "ha_automation_condition_t: неожиданный размер");
static_assert(offsetof(ha_automation_record_t, trigger_device_uid) == 8, "automation: layout trigger");
static_assert(offsetof(ha_automation_record_t, conditions) == 48, "automation: layout conditions");
static_assert(sizeof(ha_automation_record_t) == 144, "ha_automation_record_t: неожиданный размер");
static_assert(sizeof(ha_automation_record_t) <= 1024, "automation: больше региона");
#else
_Static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
_Static_assert(sizeof(ha_automation_condition_t) == 24, "ha_automation_condition_t: неожиданный размер");
_Static_assert(offsetof(ha_automation_record_t, trigger_device_uid) == 8, "automation: layout trigger");
_Static_assert(offsetof(ha_automation_record_t, conditions) == 48, "automation: layout conditions");
_Static_assert(sizeof(ha_automation_record_t) == 144, "ha_automation_record_t: неожиданный размер");
_Static_assert(sizeof(ha_automation_record_t) <= 1024, "automation: больше региона");
#endif

#ifdef __cplusplus
}
#endif
