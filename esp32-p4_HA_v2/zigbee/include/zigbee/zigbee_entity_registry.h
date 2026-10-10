#pragma once

/*
 * Zigbee entity registry (Шаг A, A0.4): bridge создаёт/восстанавливает logical Entity для
 * каждого endpoint'а устройства. Adapter-private: привязка (uid, endpoint) → entity_id
 * живёт здесь, не в generic Entity/Domain.
 *
 * entity_id детерминирован (A0.3), поэтому registry — in-RAM кэш: после reboot он пуст, но
 * вывод из той же физической привязки даёт тот же id. Persistence не требуется для identity.
 */

#include "domain/domain.h"
#include "ha_model/ha_entities.h" /* HA_ENTITY_ENTITY */
#include "ha_model/ha_entity.h"
#include "ha_model/ha_zigbee.h"
#include "sys/sys_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Резолвит (uid, endpoint) → entity_id (найти/вывести/добавить привязку) и ensure-ит Entity
 * в Domain. Идемпотентно; порядок discovery не влияет. Ошибка — явная (не создаём ложную Entity).
 */
sys_error_t zigbee_entity_ensure(domain_t *domain, ha_device_uid_t uid, uint8_t endpoint,
                                 ha_entity_id_t *out_entity_id);

/*
 * Чистый lookup (uid, endpoint) → entity_id без создания и без записи в Domain. false —
 * привязки нет. Для читателей, которым нельзя мутировать Domain (напр. тень canonical state).
 */
bool zigbee_entity_lookup(ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t *out_entity_id);

/*
 * Обратный lookup entity_id → (uid, endpoint) без мутаций Domain. false — привязки нет.
 * Нужен adapter'у для command-by-entity (A3.2): резолв цели команды.
 */
bool zigbee_entity_physical(ha_entity_id_t entity_id, ha_device_uid_t *out_uid,
                            uint8_t *out_endpoint);

#ifdef __cplusplus
}
#endif
