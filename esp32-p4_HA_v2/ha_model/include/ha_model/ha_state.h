#pragma once

/*
 * Canonical property state (Шаг A, A1.0): transport-agnostic истина о состоянии.
 *
 * Адрес свойства — (entity_id, property_id): логическая сущность (ha_entity.h) и
 * семантическое свойство (ha_properties.h). Здесь НЕТ cluster/attr/zcl_type/endpoint
 * и нет зависимостей от Zigbee: физическую координату знает только binding адаптера.
 *
 * Это форма хранения, а не логика. Raw-состояние Zigbee в Domain не живёт (Шаг C):
 * PROPERTY_STATE — единственная истина состояния, а физическую координату знает только
 * binding адаптера.
 */

#include <stdint.h>

#include "ha_model/ha_entity.h"
#include "ha_model/ha_properties.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Адрес: логическая сущность + семантическое свойство. HA_PROPERTY_UNKNOWN в ключе
 * смысла не имеет (такого свойства нет), но отдельного invalid-числа ключ не вводит.
 *
 * `reserved` — явный padding, а не неявный: Domain сравнивает ключи побайтово, поэтому
 * ключ обязан иметь детерминированные байты (иначе designated-init оставил бы мусор в
 * хвосте и lookup бы не находил запись). Держать нулём (любой `{0}`-инициализатор).
 */
typedef struct {
    ha_entity_id_t entity;
    ha_property_id_t property;
    uint32_t reserved;
} ha_state_key_t;

/*
 * Значение свойства в семантической форме (ha_value_t). Ни unit, ни диапазона, ни
 * кодировки внутри: контекст даёт ha_property_desc(property). HA_VALUE_NONE —
 * «значение отсутствует»; «не удалось получить» — отдельный исход, не подменяется.
 */
typedef struct {
    ha_value_t value;
} ha_state_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_value_t) == 8, "ha_value_t: неожиданный размер");
static_assert(sizeof(ha_state_key_t) == 16, "ha_state_key_t: неожиданный размер");
static_assert(offsetof(ha_state_key_t, property) == 8, "ha_state_key_t: layout");
static_assert(offsetof(ha_state_key_t, reserved) == 12, "ha_state_key_t: layout");
static_assert(sizeof(ha_state_record_t) == 8, "ha_state_record_t: неожиданный размер");
#else
_Static_assert(sizeof(ha_value_t) == 8, "ha_value_t: неожиданный размер");
_Static_assert(sizeof(ha_state_key_t) == 16, "ha_state_key_t: неожиданный размер");
_Static_assert(offsetof(ha_state_key_t, property) == 8, "ha_state_key_t: layout");
_Static_assert(offsetof(ha_state_key_t, reserved) == 12, "ha_state_key_t: layout");
_Static_assert(sizeof(ha_state_record_t) == 8, "ha_state_record_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
