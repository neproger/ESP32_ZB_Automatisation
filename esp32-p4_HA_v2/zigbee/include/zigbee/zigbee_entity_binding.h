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

/* Канонический Zigbee seed: [device_uid LE(8) | endpoint(1)] (9 байт), без memcpy(struct). */
bool zb_entity_seed(ha_device_uid_t uid, uint8_t endpoint, uint8_t out[9]);

/* Добавить привязку в fixed-capacity список. false — нет места/NULL. */
bool zb_entity_binding_add(zb_entity_binding_t *list, size_t capacity, size_t *count,
                           ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t entity_id);

/*
 * Resolve-or-create (A0.4): (uid, endpoint) → entity_id. Есть привязка — вернуть её;
 * нет — построить seed, вывести `ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, ...)` и
 * добавить привязку. Идемпотентно; порядок discovery не влияет. `out_created` — создана ли
 * новая привязка. false — не удалось (нет места); ложная identity НЕ создаётся.
 */
bool zb_entity_resolve(zb_entity_binding_t *list, size_t capacity, size_t *count,
                       ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t *out_entity_id,
                       bool *out_created);

#ifdef __cplusplus
}
#endif
