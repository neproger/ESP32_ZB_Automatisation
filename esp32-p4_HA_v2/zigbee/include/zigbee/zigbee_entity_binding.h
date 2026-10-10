#pragma once

/*
 * Zigbee-private binding: физическая привязка ↔ logical entity (Шаг A, A0.2,
 * docs/STEP_A_PLAN.md §3.1). Таблица Zigbee-specific, поэтому поля `transport` в ней нет.
 *
 * Здесь НЕ выводятся entity_id (это A0.3) и НЕ хранится cluster/attr (это Property, не Entity).
 * Модуль чистый (без Domain/радио) — проверяется на хосте.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_entity.h"
#include "ha_model/ha_zigbee.h" /* ha_device_uid_t */

#ifdef __cplusplus
extern "C" {
#endif

/* key = (device_uid, endpoint); record = { entity_id }. */
typedef struct {
    ha_entity_id_t entity_id;
    ha_device_uid_t device_uid;
    uint8_t endpoint;
} zb_entity_binding_t;

/* physical (uid, endpoint) → entity_id. false — нет привязки. */
bool zb_entity_binding_find_entity(const zb_entity_binding_t *list, size_t count,
                                   ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t *out);

/* entity_id → physical (uid, endpoint). false — нет привязки. */
bool zb_entity_binding_find_physical(const zb_entity_binding_t *list, size_t count,
                                     ha_entity_id_t entity_id, ha_device_uid_t *uid, uint8_t *endpoint);

#ifdef __cplusplus
}
#endif
