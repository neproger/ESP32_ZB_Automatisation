# Canonical Device/Entity/Property/Action/Event model

> Карта системы — `ARCHITECTURE.md`. Формы записей — `RECORD_MODEL.md`.
> **Статус: миграция завершена** (Steps A–F). Ниже — текущая canonical-модель, без
> переходных оговорок.

## 1. Идея

Верхние слои и бизнес-логика работают только с canonical-моделью. Физическая
репрезентация (`device_uid/endpoint/cluster/attr/zcl_type`) — adapter-owned и за границей
`zigbee/` не существует.

```text
Logical Device ── Entity ── Property (состояние)
                        ├─ Capability (что Entity умеет: properties + events)
                        ├─ Event (press-события)
                        └─ Action (намерение над Property)
```

## 2. Сущности модели

| Понятие | Identity (ключ) | Что это |
|---|---|---|
| **Logical Device** | `device_id` (opaque u64) | логический узел/вещь; дерево по `parent_device_id`; `source`+`native_id` — внешний ключ |
| **Entity** | `entity_id` (opaque u64) | логическая сущность; `device_id` — владелец (ownership) |
| **Property state** | `(entity_id, property_id)` | единственная истина состояния; значение — `ha_value_t` |
| **Capabilities** | `entity_id` | что Entity умеет: `properties[]{property, actions[]}` + `events[]` |
| **Event** | `(source_entity, event_id)` | что-то произошло, state не следует (кнопки, тики) |
| **Action** | `(property_id, action_id)` | намерение; команда адресуется парой `(entity_id, property_id)` |
| **Automation rule** | `id` (u64) | `ha_rule_t`, entity-адресованное правило |
| **Group / group_item** | `id` / `(group_id, entity_id)` | экраны Display и плитки-Entity |

## 3. Identity

- `device_id = ha_device_id_derive(source, native_id)`; `entity_id = ha_entity_id_derive(issuer, seed)`
  (SipHash-2-4, фиксированный ключ проекта). Детерминированны → перепривязка/reboot дают те же id.
- `source`/`issuer` — opaque-числа; значения объявляет адаптер (`zigbee_adapter_ids.h`).
  Generic core именует только `HA_SOURCE_NONE` и `HA_ENTITY_ISSUER_SYSTEM`.
- Внешний ключ: `native_id` (строка) — стабильная внешняя идентичность, хранится в Logical
  Device; разбор `native_id` — только в адаптере.

## 4. Значение (`ha_value_t`)

```c
typedef enum { HA_VALUE_NONE=0, HA_VALUE_BOOL, HA_VALUE_I32, HA_VALUE_U32, HA_VALUE_FLOAT, HA_VALUE_ENUM } ha_value_kind_t;
typedef struct { ha_value_kind_t kind; union { bool b; int32_t i32; uint32_t u32; float f32; uint32_t enum_value; } value; } ha_value_t;
```

- `ha_value_to_double` — числовая проекция (Automation/Display), чистая функция в `ha_model`.
- Domain Journal несёт компактное `domain_value_t` с совпадающей нумерацией — контролируемый
  ABI-boundary (Domain не зависит от `ha_model`); формы не объединяем.

## 5. Capabilities

`ha_entity_capabilities_record_t`:

```text
properties[] — { property, action_count, actions[] }   (state + command)
events[]     — ha_event_id_t[]                          (press-события)
```

Собирает мост внутри `zigbee/` из интервью endpoint'а: server-кластеры → properties,
client-кластеры → events. Одна Entity на endpoint. Web/UI берут действия/свойства/события
из capabilities Entity, а не из глобального словаря.

## 6. Триггеры и действия Automation

`ha_rule_t` (entity-native): триггер `DEVICE_EVENT`(`entity,event_id`) / `TIME`
(`minutes_of_day, weekday_mask`) / `STATE`(`entity, property, op, edge, value, value2`);
условия (AND) по `(entity, property, op, value…)`; действие — `(entity, property, action,
value_kind[, scalar|xy])`. `HA_ENTITY_ID_SOURCE` = «то же, что источник триггера».

Automation подписывается на canonical **тип факта** (не на source) и постит `HA_CMD_PROPERTY`.

## 7. Мост ZCL ↔ canonical

Внутри `zigbee/` (`zigbee_semantics.c`): таблица `(cluster, attr) → property` + стратегия
декодирования; `(property, action) → ZCL-команда`; `(cluster, command) → event id`. Наружу
не выходит. Системный сервис мост не использует — публикует canonical значения напрямую.

## 8. Провод (Web)

Canonical records **verbatim**: `type = ha_entity_t`, payload = `[key][record]`
(`REMOVE` — `[key]`). Типы — `ENTITY/LOGICAL_DEVICE/PROPERTY_STATE/CAPABILITIES/
AUTOMATION/GROUP_ITEM` + generic `LOCATION/WEATHER/WIFI_STATUS/SETTINGS/GROUP`; событие —
`EVENT` = `ha_event_t`. Раскладка и offset'ы — `services/WEB_PROTOCOL.md`. Web не строит
DTO: wire-форма совпадает с canonical layout, интерпретация — в браузере. Числовых
Domain-типов и raw-записей на проводе нет.

## 9. Было / стало (кратко)

```text
было:  canonical state = (device_uid, endpoint, cluster, attr), Zigbee-производный
стало: canonical state = (entity_id, property_id); Device/Entity/Property — transport-agnostic

было:  raw device/state/endpoint entities в Domain, WS v2 с числовым типом
стало: raw в adapter-private store; WS v3 entity-native; diagnostics удалены

было:  semantics как отдельный generic-компонент; PERMIT_JOIN в generic command
стало: мост внутри zigbee/; HA_CMD_DISCOVERY (adapter трактует как permit join)
```

История миграции — `PROPERTY_MODEL_IMPL_JOURNAL.md`.
