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

#include "ha_model/ha_capabilities.h"   /* ha_entity_capabilities_record_t */
#include "ha_model/ha_properties.h"     /* ha_property_id_t, ha_value_t, ha_command_value_t */
#include "zigbee/zb_command.h"       /* ha_zb_command_t (ZCL-private форма) */
#include "zigbee/zigbee_forms.h"     /* ha_zb_state_key_t/record, device/endpoint forms */

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

/* Числовой вид значения (для сравнения). false — HA_VALUE_NONE. */

/*
 * Физическая форма Zigbee-события (до потери cluster/payload). Маппинг в семантику
 * делается ДО публикации в Domain: событие эфемерно, persistent-ABI его не держит.
 */
typedef struct {
    uint64_t device_uid;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint8_t command_id;
    uint8_t payload_len; /* vendor-команды (Tuya OnOff 0xFD) кодируют нажатие в payload */
    uint8_t payload[8];
} ha_zb_event_t;

/*
 * Физическое событие → семантическое. Учитывает профиль устройства (device->model),
 * т.к. один command_id может значить разное у разных производителей. false — нет
 * семантики для этой пары (событие не публикуется как semantic).
 */
bool semantics_decode_event(const ha_zb_event_t *physical, const ha_device_record_t *device,
                            ha_event_t *out);

/* Semantic capabilities server-кластера: свойство + его действия (для canonical capabilities). */
size_t semantics_cluster_capabilities(uint16_t cluster_id, ha_capability_t *out, size_t max);

/*
 * Canonical interface endpoint'а: server-кластеры → свойства/действия, client-кластеры →
 * события. Bridge сам собирает обе части; adapter не разбирает кластеры (cluster/attr не
 * сохраняются). Adapter зовёт при интервью и пишет по entity_id.
 */
bool semantics_endpoint_capabilities(const ha_endpoint_record_t *endpoint,
                                     ha_entity_capabilities_record_t *out);

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
