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

#include "ha_model/ha_automation.h" /* ha_sem_rule_t, ha_automation_record_t */
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
 * Физическая форма Zigbee-события (до потери cluster/payload). Маппинг в семантику
 * делается ДО публикации в Domain: событие эфемерно, persistent-ABI его не держит.
 */
#define ZB_EVENT_PAYLOAD_MAX 16

typedef struct {
    uint64_t device_uid;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint8_t command_id;
    uint8_t payload_len;
    uint8_t payload[ZB_EVENT_PAYLOAD_MAX];
} ha_zb_event_t;

/*
 * Физическое событие → семантическое. Учитывает профиль устройства (device->model),
 * т.к. один command_id может значить разное у разных производителей. false — нет
 * семантики для этой пары (событие не публикуется как semantic).
 */
bool semantics_decode_event(const ha_zb_event_t *physical, const ha_device_record_t *device,
                            ha_event_t *out);

/*
 * Capabilities для BFF/UI: какие semantic-свойства отдаёт кластер (server) и какие
 * действия поддерживает свойство. Заменяет inference UI по cluster/attr.
 * Возвращают число записанных (<= max).
 */
size_t semantics_cluster_properties(uint16_t cluster_id, ha_property_id_t *out, size_t max);
size_t semantics_property_actions(ha_property_id_t property, ha_action_id_t *out, size_t max);

/*
 * Reverse: семантическое событие → legacy command_id по профилю устройства. false —
 * событие неоднозначно/невыразимо (напр. SINGLE_PRESS от общего профиля = ON/OFF/TOGGLE).
 */
bool semantics_event_to_physical(const ha_device_record_t *device, ha_event_id_t event,
                                 uint16_t *out_command_id);

/*
 * Скомпилировать semantic-правило в physical record (один раз, при сохранении из UI).
 * trigger_device — профиль устройства-источника (для reverse event; может быть NULL).
 * false — правило невыразимо в legacy ABI (неоднозначный event, BETWEEN на STATE-триггере,
 * неизвестный property/action). Вызывающий сообщает понятную ошибку, а не сохраняет неверное.
 */
bool semantics_compile_automation(const ha_sem_rule_t *rule, const ha_device_record_t *trigger_device,
                                  ha_automation_record_t *out);

/*
 * Обратная проекция: physical record → semantic rule (для read-path UI). false — правило
 * невыразимо семантически (legacy: неоднозначный event, неизвестная пара, неподъёмный
 * action args). Тогда Web/UI показывает его как legacy/raw, не выдумывая смысл.
 */
bool semantics_decompile_automation(const ha_automation_record_t *record,
                                    const ha_device_record_t *trigger_device, ha_sem_rule_t *out);

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
