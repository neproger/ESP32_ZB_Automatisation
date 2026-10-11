#pragma once

/*
 * Canonical capabilities (Шаг A, A6.1b; Шаг D — полный interface Entity): что умеет
 * логическая Entity. Две независимые части:
 *   properties — свойства и допустимые над ними действия (state + command);
 *   events     — press-события, которые Entity может породить.
 * Ключ — entity_id; cluster/attr/command в записи не попадают. Форма — фиксированный POD
 * (payload_size Domain). Событие не моделируется как property без state — это разные оси.
 */

#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_entity.h"
#include "ha_model/ha_properties.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA_ENTITY_CAPABILITIES_MAX 8
#define HA_ENTITY_EVENTS_MAX 4

typedef struct {
    uint8_t property_count;
    uint8_t reserved[3];
    ha_capability_t properties[HA_ENTITY_CAPABILITIES_MAX];
    uint8_t event_count;
    uint8_t events_reserved[3];
    ha_event_id_t events[HA_ENTITY_EVENTS_MAX];
} ha_entity_capabilities_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_capability_t) == 24, "ha_capability_t: неожиданный размер");
static_assert(sizeof(ha_entity_capabilities_record_t) == 216,
              "ha_entity_capabilities_record_t: неожиданный размер");
static_assert(offsetof(ha_entity_capabilities_record_t, events) == 200,
              "capabilities: events layout");
#else
_Static_assert(sizeof(ha_capability_t) == 24, "ha_capability_t: неожиданный размер");
_Static_assert(sizeof(ha_entity_capabilities_record_t) == 216,
               "ha_entity_capabilities_record_t: неожиданный размер");
_Static_assert(offsetof(ha_entity_capabilities_record_t, events) == 200,
               "capabilities: events layout");
#endif

#ifdef __cplusplus
}
#endif
