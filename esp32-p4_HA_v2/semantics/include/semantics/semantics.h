#pragma once

/*
 * Мост «ZCL ↔ семантика» (docs/PROPERTY_MODEL.md, фаза 1). Единственный, кто знает
 * пары (cluster, attr) и их транспортную кодировку. Потребитель передаёт physical
 * reference как opaque-адрес и получает семантику; сам cluster/attr он не толкует.
 *
 * Публичный API — только семантический: вход — физический ключ состояния, выход —
 * ha_property_id_t / ha_value_t. Таблица (cluster, attr) и стратегия декодирования
 * наружу не выходят (иначе Zigbee протёк бы в потребителя).
 */

#include <stdbool.h>

#include "ha_model/ha_commands.h"   /* ha_zb_command_t */
#include "ha_model/ha_entities.h"   /* ha_zb_state_key_t, ha_zb_state_record_t */
#include "ha_model/ha_properties.h" /* ha_property_id_t, ha_value_t, ha_command_value_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Семантика состояния по физическому ключу (device/endpoint не влияют на свойство). */
ha_property_id_t semantics_property_from_key(const ha_zb_state_key_t *key);

/*
 * Декодировать состояние в семантическое значение. false — нет маппинга для пары
 * (cluster, attr), тип значения вне словаря скаляров или некорректное raw (напр.
 * illuminance = invalid). В out при false ничего валидного не кладётся.
 */
bool semantics_state_value(const ha_zb_state_key_t *key, const ha_zb_state_record_t *state,
                           ha_value_t *out);

/*
 * Обратный маппинг: физический ключ свойства в том же объекте (device/endpoint берутся из
 * context). Потребитель не собирает cluster/attr сам. false — свойство не замаплено.
 */
bool semantics_property_key(const ha_zb_state_key_t *context, ha_property_id_t property,
                            ha_zb_state_key_t *out);

/* Числовой вид значения (для сравнения). false — HA_VALUE_NONE. */
bool semantics_value_to_double(const ha_value_t *value, double *out);

/*
 * Собрать физическую команду из семантического запроса: target(device/endpoint) +
 * property + action (+ value). Транспортная кодировка (scale, transition, direction,
 * LE, args_len) принадлежит мосту. false — нет такого (property, action) или value
 * не той формы (SCALAR vs XY).
 */
bool semantics_build_command(const ha_zb_state_key_t *target, ha_property_id_t property,
                             ha_action_id_t action, const ha_command_value_t *value,
                             ha_zb_command_t *out);

#ifdef __cplusplus
}
#endif
