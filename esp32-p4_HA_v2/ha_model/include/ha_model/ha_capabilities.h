#pragma once

/*
 * Canonical capabilities (Шаг A, A6.1b): что умеет логическая Entity — свойства и
 * доступные над ними действия. Ключ — entity_id; cluster/attr в записи не попадают
 * (транспорт их не проецирует). Форма — фиксированный POD (payload_size Domain).
 */

#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_entity.h"
#include "ha_model/ha_properties.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA_ENTITY_CAPABILITIES_MAX 8

typedef struct {
    uint8_t count;
    uint8_t reserved[3];
    ha_capability_t caps[HA_ENTITY_CAPABILITIES_MAX];
} ha_entity_capabilities_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_capability_t) == 24, "ha_capability_t: неожиданный размер");
static_assert(sizeof(ha_entity_capabilities_record_t) == 196,
              "ha_entity_capabilities_record_t: неожиданный размер");
#else
_Static_assert(sizeof(ha_capability_t) == 24, "ha_capability_t: неожиданный размер");
_Static_assert(sizeof(ha_entity_capabilities_record_t) == 196,
               "ha_entity_capabilities_record_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
