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

#include "ha_model/ha_properties.h"
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
 * событие); TIME — «будильник»: конкретное время суток + набор дней недели; STATE —
 * изменение атрибута состояния по условию (device/cluster/attr/op/value + edge).
 */
typedef enum {
    HA_TRIGGER_DEVICE_EVENT = 0,
    HA_TRIGGER_TIME = 1,
    HA_TRIGGER_STATE = 2,
} ha_automation_trigger_kind_t;

/* Когда срабатывать относительно изменения атрибута (STATE). */
typedef enum {
    HA_TRIGGER_EDGE_ANY = 0,     /* при любом изменении, когда условие верно */
    HA_TRIGGER_EDGE_RISING = 1,  /* условие стало истинным (было ложно) */
    HA_TRIGGER_EDGE_FALLING = 2, /* условие стало ложным (было истинно) */
} ha_automation_trigger_edge_t;

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
    /* Диапазон: value <= actual <= value2 (оба порога — в человеческих единицах). */
    HA_CONDITION_OP_BETWEEN = 8,
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
    float value;  /* порог; для BETWEEN — нижняя граница */
    float value2; /* BETWEEN: верхняя граница (занимает выравнивающий паддинг) */
} ha_automation_condition_t;

/*
 * Параметры триггера разложены в те же 20 байт (offset 4..23) для всех видов — провод
 * (WEB_PROTOCOL §5) не меняется между видами. Два union'а: @4..7 (TIME/STATE-порог) и
 * @8..23 (устройство + специфика вида). device_uid == 0 у EVENT — «любое».
 */
typedef struct {
    uint8_t enabled;
    uint8_t action_args_len;
    uint8_t conditions_count;
    uint8_t trigger_kind; /* ha_automation_trigger_kind_t */

    union { /* @4..7 */
        struct { /* TIME */
            uint16_t minutes_of_day; /* 0..1439 */
            uint8_t weekday_mask;    /* бит 0=Пн..6=Вс */
            uint8_t reserved;
        } time;
        float state_value; /* STATE: порог сравнения */
    } trigger_a;

    union { /* @8..23 */
        struct { /* DEVICE_EVENT */
            ha_device_uid_t device_uid;
            uint16_t command_id;
            uint8_t reserved[6];
        } event;
        struct { /* STATE */
            ha_device_uid_t device_uid;
            uint8_t endpoint; /* 0 — любой */
            uint8_t reserved0;
            uint16_t cluster_id;
            uint16_t attr_id;
            uint8_t op;   /* ha_condition_op_t */
            uint8_t edge; /* ha_automation_trigger_edge_t */
        } state;
    } trigger_b;

    /* Действие: Zigbee-команда. device_uid == 0 — устройство-источник события. */
    ha_device_uid_t action_device_uid;
    uint8_t action_endpoint;
    uint16_t action_cluster_id;
    uint8_t action_command_id;
    uint8_t action_args[HA_AUTOMATION_ARGS_MAX];

    /* Условия (AND); первые conditions_count значимы. */
    ha_automation_condition_t conditions[HA_AUTOMATION_CONDITIONS_MAX];
} ha_automation_record_t;

/*
 * Семантическая форма правила (фаза 5.3): то, чем оперирует UI/BFF. Компилируется
 * один раз в physical `ha_automation_record_t` (transitional ABI). Здесь нет cluster/
 * attr/command id — только property/action/value; physical-координаты даёт мост.
 */
typedef struct {
    ha_device_uid_t device_uid; /* 0 — устройство-источник (в условии/действии) */
    uint8_t endpoint;           /* 0 — любой */
    ha_property_id_t property;
} ha_sem_ref_t;

typedef struct {
    uint8_t kind; /* ha_automation_trigger_kind_t */
    /* EVENT */
    ha_device_uid_t device_uid;
    ha_event_id_t event_id;
    /* TIME */
    uint16_t minutes_of_day;
    uint8_t weekday_mask;
    /* STATE */
    uint8_t endpoint;
    ha_property_id_t property;
    uint8_t op;    /* ha_condition_op_t */
    uint8_t edge;  /* ha_automation_trigger_edge_t */
    float value;   /* STATE-порог (нижняя граница для BETWEEN) */
    float value2;  /* BETWEEN: верхняя граница */
} ha_sem_trigger_t;

typedef struct {
    ha_sem_ref_t ref;
    uint8_t op; /* ha_condition_op_t */
    float value;
    float value2;
} ha_sem_condition_t;

typedef struct {
    ha_sem_ref_t target;
    ha_action_id_t action;
    uint8_t value_kind; /* HA_COMMAND_VALUE_* */
    ha_value_t value;   /* SCALAR */
    float x;            /* XY */
    float y;
} ha_sem_action_t;

typedef struct {
    uint8_t enabled;
    ha_sem_trigger_t trigger;
    uint8_t conditions_count;
    ha_sem_condition_t conditions[HA_AUTOMATION_CONDITIONS_MAX];
    ha_sem_action_t action;
} ha_sem_rule_t;

#ifdef __cplusplus
static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
static_assert(sizeof(ha_automation_condition_t) == 24, "ha_automation_condition_t: неожиданный размер");
static_assert(offsetof(ha_automation_condition_t, value2) == 20, "automation: cond value2");
static_assert(offsetof(ha_automation_record_t, trigger_a) == 4, "automation: layout trigger_a");
static_assert(offsetof(ha_automation_record_t, trigger_b) == 8, "automation: layout trigger_b");
static_assert(offsetof(ha_automation_record_t, trigger_b.event.command_id) == 16, "automation: event cmd");
static_assert(offsetof(ha_automation_record_t, trigger_b.state.cluster_id) == 18, "automation: state cluster");
static_assert(offsetof(ha_automation_record_t, trigger_b.state.op) == 22, "automation: state op");
static_assert(offsetof(ha_automation_record_t, action_device_uid) == 24, "automation: action");
static_assert(offsetof(ha_automation_record_t, conditions) == 48, "automation: layout conditions");
static_assert(sizeof(ha_automation_record_t) == 144, "ha_automation_record_t: неожиданный размер");
static_assert(sizeof(ha_automation_record_t) <= 1024, "automation: больше региона");
#else
_Static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
_Static_assert(sizeof(ha_automation_condition_t) == 24, "ha_automation_condition_t: неожиданный размер");
_Static_assert(offsetof(ha_automation_condition_t, value2) == 20, "automation: cond value2");
_Static_assert(offsetof(ha_automation_record_t, trigger_a) == 4, "automation: layout trigger_a");
_Static_assert(offsetof(ha_automation_record_t, trigger_b) == 8, "automation: layout trigger_b");
_Static_assert(offsetof(ha_automation_record_t, trigger_b.event.command_id) == 16, "automation: event cmd");
_Static_assert(offsetof(ha_automation_record_t, trigger_b.state.cluster_id) == 18, "automation: state cluster");
_Static_assert(offsetof(ha_automation_record_t, trigger_b.state.op) == 22, "automation: state op");
_Static_assert(offsetof(ha_automation_record_t, action_device_uid) == 24, "automation: action");
_Static_assert(offsetof(ha_automation_record_t, conditions) == 48, "automation: layout conditions");
_Static_assert(sizeof(ha_automation_record_t) == 144, "ha_automation_record_t: неожиданный размер");
_Static_assert(sizeof(ha_automation_record_t) <= 1024, "automation: больше региона");
#endif

#ifdef __cplusplus
}
#endif
