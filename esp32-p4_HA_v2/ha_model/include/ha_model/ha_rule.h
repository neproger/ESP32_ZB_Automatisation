#pragma once

/*
 * Хранимая форма правила автоматизации (Шаг A, A3). Заменяет physical
 * `ha_automation_record_t` (uid/ep/cluster/attr/command) на entity-адресованную
 * семантическую форму: Domain знает только (entity_id, property_id), а транспортом
 * распоряжается adapter (command-by-entity, A3.2).
 *
 * Отличие от transitional `ha_sem_rule_t`: ссылки — entity_id, а не device_uid+endpoint.
 * «то же устройство, что вызвало» — сентинел HA_ENTITY_ID_SOURCE (резолвится при eval).
 *
 * Запись — фиксированный POD (payload_size Domain фиксирован). Все хвосты — явные
 * reserved: байтовое представление детерминировано (Domain сравнивает записи побайтово).
 */

#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_automation.h" /* ha_condition_op_t, trigger kind/edge */
#include "ha_model/ha_entity.h"     /* ha_entity_id_t, HA_ENTITY_ID_SOURCE */
#include "ha_model/ha_properties.h" /* ha_property_id_t, ha_event_id_t, ha_command_value_t */

#ifdef __cplusplus
extern "C" {
#endif

#define HA_RULE_CONDITIONS_MAX 4

/* Адрес в правиле: (entity, property). entity может быть HA_ENTITY_ID_SOURCE. */
typedef struct {
    ha_entity_id_t entity;
    ha_property_id_t property;
    uint32_t reserved;
} ha_rule_ref_t;

/* Сравнение: оператор + порог(и). value2 — верхняя граница BETWEEN. */
typedef struct {
    uint8_t op; /* ha_condition_op_t */
    uint8_t reserved[3];
    float value;
    float value2;
} ha_rule_compare_t;

typedef struct {
    ha_rule_ref_t ref;
    ha_rule_compare_t cmp;
    uint32_t reserved;
} ha_rule_condition_t;

/*
 * Триггер: разложен на все виды (без union — детерминированные байты). Значимы поля по
 * kind: EVENT — entity + event_id; STATE — entity + property + op + edge + value(+value2);
 * TIME — minutes_of_day + weekday_mask.
 */
typedef struct {
    ha_entity_id_t entity;     /* EVENT/STATE; HA_ENTITY_ID_SOURCE = источник */
    ha_event_id_t event_id;    /* EVENT */
    ha_property_id_t property; /* STATE */
    float value;               /* STATE: порог (нижняя граница BETWEEN) */
    float value2;              /* STATE: верхняя граница BETWEEN */
    uint16_t minutes_of_day;   /* TIME: 0..1439 */
    uint8_t kind;              /* ha_automation_trigger_kind_t */
    uint8_t op;                /* STATE: ha_condition_op_t */
    uint8_t edge;              /* STATE: ha_automation_trigger_edge_t */
    uint8_t weekday_mask;      /* TIME: бит 0=Пн..6=Вс */
    uint8_t reserved[2];
} ha_rule_trigger_t;

typedef struct {
    ha_rule_ref_t action_ref;   /* цель; HA_ENTITY_ID_SOURCE → цель = источник триггера */
    ha_rule_trigger_t trigger;
    ha_rule_condition_t conditions[HA_RULE_CONDITIONS_MAX];
    ha_command_value_t action_value; /* SCALAR (ha_value_t) или XY; kind NONE — без значения */
    uint8_t enabled;
    uint8_t conditions_count;
    uint8_t action; /* ha_action_id_t */
    uint8_t reserved0;
} ha_rule_t;

#ifdef __cplusplus
static_assert(sizeof(ha_rule_ref_t) == 16, "ha_rule_ref_t: неожиданный размер");
static_assert(sizeof(ha_rule_compare_t) == 12, "ha_rule_compare_t: неожиданный размер");
static_assert(sizeof(ha_rule_condition_t) == 32, "ha_rule_condition_t: неожиданный размер");
static_assert(sizeof(ha_rule_trigger_t) == 32, "ha_rule_trigger_t: неожиданный размер");
static_assert(sizeof(ha_command_value_t) == 12, "ha_command_value_t: неожиданный размер");
static_assert(sizeof(ha_rule_t) == 192, "ha_rule_t: неожиданный размер");
static_assert(offsetof(ha_rule_t, trigger) == 16, "ha_rule_t: layout trigger");
static_assert(offsetof(ha_rule_t, conditions) == 48, "ha_rule_t: layout conditions");
static_assert(offsetof(ha_rule_t, action_value) == 176, "ha_rule_t: layout action_value");
static_assert(offsetof(ha_rule_t, enabled) == 188, "ha_rule_t: layout enabled");
static_assert(offsetof(ha_rule_t, action) == 190, "ha_rule_t: layout action");
static_assert(sizeof(ha_rule_t) <= 1024, "ha_rule_t: больше региона");
#else
_Static_assert(sizeof(ha_rule_ref_t) == 16, "ha_rule_ref_t: неожиданный размер");
_Static_assert(sizeof(ha_rule_compare_t) == 12, "ha_rule_compare_t: неожиданный размер");
_Static_assert(sizeof(ha_rule_condition_t) == 32, "ha_rule_condition_t: неожиданный размер");
_Static_assert(sizeof(ha_rule_trigger_t) == 32, "ha_rule_trigger_t: неожиданный размер");
_Static_assert(sizeof(ha_command_value_t) == 12, "ha_command_value_t: неожиданный размер");
_Static_assert(sizeof(ha_rule_t) == 192, "ha_rule_t: неожиданный размер");
_Static_assert(offsetof(ha_rule_t, trigger) == 16, "ha_rule_t: layout trigger");
_Static_assert(offsetof(ha_rule_t, conditions) == 48, "ha_rule_t: layout conditions");
_Static_assert(offsetof(ha_rule_t, action_value) == 176, "ha_rule_t: layout action_value");
_Static_assert(offsetof(ha_rule_t, enabled) == 188, "ha_rule_t: layout enabled");
_Static_assert(offsetof(ha_rule_t, action) == 190, "ha_rule_t: layout action");
_Static_assert(sizeof(ha_rule_t) <= 1024, "ha_rule_t: больше региона");
#endif

#ifdef __cplusplus
}
#endif
