#pragma once

/*
 * Zigbee-private device binding: Logical Device ↔ физическое Zigbee-устройство (Шаг B, B3,
 * docs/STEP_B_PLAN.md). Единственный мост `device_id ↔ EUI64`; отдельно от entity binding
 * (`entity_id ↔ EUI64 + endpoint`). Adapter-private: наружу (в Domain) не выходит.
 *
 * device_id детерминирован (zb_device_id), поэтому registry — in-RAM кэш: после reboot он
 * пуст, но выводится из той же EUI64 заново. Persistence не требуется для identity.
 *
 * Модуль чистый (без Domain/радио) — проверяется на хосте.
 */

#include <stdbool.h>
#include <stddef.h>

#include "ha_model/ha_logical_device.h" /* ha_device_id_t, HA_DEVICE_ID_NONE */
#include "ha_model/ha_zigbee.h"         /* ha_device_uid_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ha_device_id_t device_id;
    ha_device_uid_t device_uid;
} zb_device_binding_t;

/* EUI64 → device_id. false — нет привязки. */
bool zb_device_binding_find_device_id(const zb_device_binding_t *list, size_t count,
                                      ha_device_uid_t uid, ha_device_id_t *out);

/* device_id → EUI64. false — нет привязки. */
bool zb_device_binding_find_uid(const zb_device_binding_t *list, size_t count,
                                ha_device_id_t device_id, ha_device_uid_t *out);

/*
 * Добавить пару. Идемпотентно: повтор той же пары — true. Конфликт (uid уже привязан к
 * другому device_id, или device_id к другому uid), device_id == NONE или нет места — false.
 */
bool zb_device_binding_add(zb_device_binding_t *list, size_t capacity, size_t *count,
                           ha_device_id_t device_id, ha_device_uid_t uid);

/* Удалить привязку по EUI64. false — не найдено. */
bool zb_device_binding_remove(zb_device_binding_t *list, size_t *count, ha_device_uid_t uid);

#ifdef __cplusplus
}
#endif
