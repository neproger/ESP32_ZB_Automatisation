# Step A — Final State

Краткая фиксация модели после завершения шага A (миграция к transport-agnostic
семантической модели). Детали — `PROPERTY_MODEL.md`, `STEP_A_PLAN.md`,
`PROPERTY_MODEL_IMPL_JOURNAL.md`.

## Инварианты

- **Physical identity — adapter-private.** Связь `entity_id ↔ (uid, endpoint)` живёт
  только в Zigbee adapter (binding registry). Domain и потребители знают лишь `entity_id`.
- **Logical identity — `entity_id`** (`uint64`, детерминированно из binding; `0` — none,
  `~0` — сентинел SOURCE).
- **Production state — `(entity_id, property_id) → ha_value_t`** (`HA_ENTITY_PROPERTY_STATE`).
  Единственный источник истины состояния.
- **Commands target entity/property/action** (`ha_property_command_t` → `HA_CMD_PROPERTY`).
- **Events originate from `entity_id`** (`ha_event_t.source_entity`).
- **Transport-специфичное состояние — только для diagnostics** (`HA_ENTITY_ZB_DIAG_STATE`,
  канал `WEB_MSG_DIAG_ZB_*`, вне WS v3).
- **Domain-ключи детерминированы побайтово** (неявного padding нет; либо без padding, либо
  явные `reserved`, всегда zero-initialized).

## Слои

```
Domain (canonical)
  Entity(id, name)
  Property State(entity, property) → ha_value_t
  Capabilities(entity)
  Automation(ha_rule_t: entity/property/event/action)
  Group Item(group, entity, property)
  Event(source_entity, event_id, value)

Services (только entity/property)
  Automation / Display / Web

Adapters
  Zigbee  physical ↔ entity (binding + ZCL↔semantics bridge)
  System  пишет canonical PROPERTY_STATE для ENTITY_SYSTEM
```

## WS v3 (canonical)

Read: `ENTITY`, `PROPERTY_STATE`, `CAPABILITIES`, `AUTOMATION`, `GROUP_ITEM`, `EVENT`
(+`*_REMOVE`). Commands: `PROPERTY_COMMAND`, `ENTITY_RENAME`, `AUTOMATION_PUT/REMOVE`,
`GROUP_ITEM_PUT/REMOVE`. Ни одно v3-сообщение не несёт `uid/endpoint/cluster/attr/zcl_type/raw`.

## Diagnostics

`WEB_MSG_DIAG_ZB_DEVICE/ENDPOINT/STATE/REMOVE` — transport-specific ABI (uid/ep/cluster/attr/
zcl_type/raw допустимы). `Diagnostics.jsx` — read-only, единственный потребитель raw.

## Не-Entity подсистемы

`Weather`/`Location`/`WiFi` остаются отдельными configuration/status-сущностями со своим
жизненным циклом и не обязаны становиться Entity. Перевод в Entity — только если появятся
properties/actions/events, нужные Automation/Groups/UI (например, погода со свойствами
TEMPERATURE/HUMIDITY/PRESSURE/CONDITION).

## Migration / rollback

Раскладка persistent-таблиц менялась (`automation_v2`, `group_item_v2`; raw-state →
ZB_DIAG_STATE). На первом boot новой прошивки раздел `mstore` разово стирается (marker в NVS,
`app_main.c:erase_legacy_mstore_once`). **Откат на старый ABI не поддерживается** — это точка
невозврата.
