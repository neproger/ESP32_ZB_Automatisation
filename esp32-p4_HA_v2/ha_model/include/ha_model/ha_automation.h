#pragma once

/*
 * Словарь правила автоматизации (docs/services/AUTOMATION.md). Только перечисления и
 * ключ: форма правила (Шаг A, A3) — entity-адресованная `ha_rule_t` (ha_model/ha_rule.h).
 * Physical-форма (ha_automation_record_t) удалена вместе с compile/decompile моста.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ключ правила — числовой id: правило адресуется независимо от устройства. */
typedef struct {
    uint64_t id;
} ha_automation_key_t;

/*
 * Вид триггера. DEVICE_EVENT — событие от источника (semantic event id); TIME —
 * «будильник» (время суток + дни недели); STATE — изменение свойства по условию.
 */
typedef enum {
    HA_TRIGGER_DEVICE_EVENT = 0,
    HA_TRIGGER_TIME = 1,
    HA_TRIGGER_STATE = 2,
} ha_automation_trigger_kind_t;

/* Когда срабатывать относительно изменения свойства (STATE). */
typedef enum {
    HA_TRIGGER_EDGE_ANY = 0,     /* при любом изменении, когда условие верно */
    HA_TRIGGER_EDGE_RISING = 1,  /* условие стало истинным (было ложно) */
    HA_TRIGGER_EDGE_FALLING = 2, /* условие стало ложным (было истинно) */
} ha_automation_trigger_edge_t;

/* Оператор сравнения (условия и STATE-триггер). */
typedef enum {
    HA_CONDITION_OP_EQ = 1,
    HA_CONDITION_OP_NE = 2,
    HA_CONDITION_OP_GT = 3,
    HA_CONDITION_OP_LT = 4,
    HA_CONDITION_OP_GE = 5,
    HA_CONDITION_OP_LE = 6,
    /* Битовая маска: (value & actual) != 0. */
    HA_CONDITION_OP_HAS_BITS = 7,
    /* Диапазон: value <= actual <= value2. */
    HA_CONDITION_OP_BETWEEN = 8,
} ha_condition_op_t;

#ifdef __cplusplus
static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
#else
_Static_assert(sizeof(ha_automation_key_t) == 8, "ha_automation_key_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
