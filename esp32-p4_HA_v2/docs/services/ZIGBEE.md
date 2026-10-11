# Zigbee Adapter

Слой: **Adapters / Zigbee**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; реализация — в коде (`zigbee/`).

Как адаптер вызывает Domain — `../domain/DOMAIN_API.md`. Canonical-формы — `../RECORD_MODEL.md`.
Словарь ZCL и мост «ZCL ↔ canonical» живут **внутри `zigbee/`**: `zigbee/include/zigbee/ha_zigbee.h`,
`zigbee_forms.h`, `zb_command.h`, `zigbee_semantics.{h,c}`. Интерпретация устройств и проекция
возможностей — `ZIGBEE_CAPABILITIES.md`.

## 1. Роль

Zigbee — **полностью внутренний адаптер**. Наружу отдаёт только canonical contract
(`Device/Entity/Property/Event/Capability`, executor команды `HA_CMD_PROPERTY`); весь
транспортный словарь (`EUI64/endpoint/cluster/attribute/ZCL`, сетевой адрес, интервью,
bindings, radio) не выходит за `zigbee/`.

```text
по репорту/событию:  radio → private store → bridge (ZCL→canonical) → Domain (canonical факт)
по команде:          Domain → executor → resolve entity→addr → bridge (canonical→ZCL) → radio
по удалению:         Device service remover(source=ZIGBEE) → deferred leave
```

## 2. Разделение данных: adapter vs Domain

| Данные | Где живёт | Почему |
|---|---|---|
| EUI-64, endpoints, кластеры с ролями, имя/модель устройства | Zigbee-private RAM store (`zigbee_device_store`) | транспортное состояние интервью; наружу не выходит |
| пометка «удалить» (deferred leave) | Zigbee-private NVS (`zigbee_remove_mark`) | переживает reboot; generic Domain не знает |
| mapping `(uid, endpoint) ↔ entity_id` | Zigbee-private RAM registry (`zigbee_entity_registry`) | adapter-private; identity детерминирована → восстанавливается |
| canonical состояние | Domain `PROPERTY_STATE` `(entity_id, property)` | единственная истина для всех потребителей |
| canonical Device/Entity/Capabilities | Domain | canonical модель |
| `source`/`issuer`/`sys_layer` адаптера | `zigbee_adapter_ids.h` | generic core их не именует |

Адаптер не держит canonical state как истину; его RAM-структуры производные и восстановимы.

## 3. Приватная модель (внутри адаптера)

```text
device (EUI-64)                       — zigbee_device_store
  └── endpoint (profile, device_id)   — topology
        └── cluster (role: client|server)
```

- identity — `ha_device_uid_t` (EUI-64), не short address (меняется при перепривязке);
- `zigbee_device_store` — RAM: имя/модель + топология endpoint'ов (cluster+role);
  интервью повторяется на каждом старте, persistence не нужна.

## 4. Мост «ZCL ↔ canonical» (внутри zigbee)

`zigbee_semantics.c` — единственное место, где числа ZCL получают смысл:

- `semantics_property_from_key` / `semantics_state_value` — `(cluster,attr)+zcl_type` →
  `(property_id, ha_value_t)`;
- `semantics_build_command` — `property+action(+value)` → `ha_zb_command_t`;
- `semantics_decode_event` — `(cluster,command)` → `ha_event_id_t`;
- `semantics_endpoint_capabilities` — endpoint → canonical `Capabilities`
  (server-кластеры → properties/actions, client-кластеры → events).

Поверх этого моста адаптер говорит с Domain только canonical DTO.

## 5. Репорт → canonical состояние

```text
1. ZCL Report Attributes (0x0A) в задаче радио
2. device по EUI-64 известен? (zigbee_device_has) — иначе репорт не факт
3. bridge: декод значения в (property, ha_value_t)
4. domain_entity_put(HA_ENTITY_PROPERTY_STATE, {entity,property}, {value})
5. changed == false → факта нет, подписчики не будятся
```

- сырое состояние (`ha_zb_state_record_t`) в Domain **не** хранится (истина — canonical);
- «устройство живо» — не факт: диагностике место в счётчиках и логе.

## 6. Событие → canonical EVENT

Входящая cluster-specific команда (кнопка/пульт) — сырой ZCL-кадр → bridge даёт
`ha_event_id_t` → публикуется canonical событие (payload = `ha_event_t`,
`source_entity` из binding). Payload события (сырые байты команды) не читается — носится
только canonical event.

## 7. Команда

```text
Automation / Web
  → domain_post(HA_CMD_PROPERTY, &ha_property_command_t)
  → executor: zigbee_entity_physical(entity) → (uid, endpoint)
              bridge: semantics_build_command → ha_zb_command_t
              очередь отправки
  → Journal: COMMAND_SENT
  → задача адаптера: zigbee_radio_send
```

- executor короткий: валидация + постановка в очередь; радио из чужого контекста не трогает;
- `COMMAND_SENT` = «передали исполнителю»; подтверждение — только репортом;
- адрес (uid/endpoint) и short address — внутренняя политика адаптера, в контракте их нет.

## 8. Удаление устройства (deferred leave)

```text
UI: LOGICAL_DEVICE_REMOVE(device_id)
  → Device service: find remover по source device-а, вызов
  → zigbee_device_remover: native_id → uid → remove_mark_add(uid) + leave (best-effort)
  → успех → Domain cascade: снять ownership Entity, PROPERTY_STATE, CAPABILITIES, GROUP_ITEM refs;
            Automation refs сохраняются
  → на завершение leave: zigbee_device_remove (private store) + entity_forget + mark clear
```

Единственный путь удаления. Устройство offline → пометка ждёт announce (NVS переживает reboot).

## 9. Возможности endpoint'а

`semantics_endpoint_capabilities` собирает canonical interface: server-кластеры →
`properties[]` (property + allowed actions), client-кластеры → `events[]` (press-события).
Кнопка, у которой нет server-кластеров, получает `properties=[]`, `events=[...]`.
Публикуется адаптером по `entity_id` при интервью.

## 10. Ошибки и диагностика

- слой `ZIGBEE_SYS_LAYER` (adapter-owned) в `zigbee_adapter_ids.h`;
- ошибку нижележащего слоя не перекодирует;
- не ошибка: отсутствие репорта, повтор значения, неизвестный атрибут;
- `zigbee_diag_record()` — счётчики по видам сбоя, тег `zigbee.diag`.

## 11. Вне scope документа

Комиссионирование координатора (formation/open/steering), интервью (`Active_EP`/`Simple_Desc`
+ Basic), подписка (bind server-кластеров + Configure Reporting), radio spinel→`ot_rcp`,
события входящих команд, уход по сетевому leave — реализовано в задаче радио.

```text
открытые: политика повторов/таймаутов интервью; удаление offline-устройства (deferred leave);
groups/scenes/OTA; пользовательские bindings; mesh-маршрутизация; read_attr/network scan
```

## 12. Порядок реализации

```text
Steps A–C: canonical identity/state, граница адаптера, приватный store, удаление diagnostics
Step D:    events в capabilities (server→properties, client→events)
Step E:    canonical event-факт (ha_event_t), wire v3 для generic-сущностей
Step F:    мост semantics внутрь zigbee; adapter-owned ids; DISCOVERY вместо permit-join
```

Все пункты выполнены; открытые вопросы — от конкретных устройств (composition, словарь кластеров).
