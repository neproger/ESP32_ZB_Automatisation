# Step C — Transport Boundary Cleanup

**Статус: C1, C2, C6, C8 сделаны; C3/C4/C5/C7 выполнены одним связным коммитом.** Диагностику
решено удалить целиком (устройство видно в списке Devices); функция никому не нужна. Raw
адаптера (имя/модель устройства + топология endpoint'ов) переведён в Zigbee-private RAM-store;
raw state удалён.

Цель: **Zigbee/ZCL существует только внутри `components/zigbee` и моста `semantics`**. Снаружи
(production generic core: `ha_model`, `domain`, `device`, `automation`, `web`, `display`,
`system`, `wifi`) система знает только `Device / Entity / Property / Action / Event /
Capability` и `source / native_id`. Никаких `EUI64 / endpoint / cluster / attr / zcl_type`,
никаких Zigbee-specific Domain-сущностей и команд.

**Исключение — `semantics`.** Это transport translation layer (мост «физическая
Zigbee-репрезентация ↔ generic Property/Action/Event»), а не application layer: ей одной
разрешено знать обе стороны и держать ZCL-словарь (`semantics/ha_zigbee.h`) и физические
формы (`semantics/zigbee_forms.h`, `semantics/zb_command.h`).

Отличие от Step B: B вернул ownership/Device, но Zigbee-ABI всё ещё лежит в общем `ha_model`
и в Domain registry (`HA_ENTITY_ZB_DIAG_*`, `HA_ENTITY_ENDPOINT`, `ha_zb_*`, `HA_CMD_ZIGBEE_*`).
Step C вырезает этот legacy, а не прячет под `DIAG_*`.

## Целевые слои

```
Application:  Automation / Web / Display   (только Domain + generic core)
Domain
Device / Entity / Property / Capability / Event / Action / Automation / Group / State
Adapter contract:  ha_device_desc_t, ha_entity_id_t, ha_property_command_t, ha_event_t, ha_value_t
Adapters:  Zigbee  |  GPIO  |  System
```

Zigbee внутри: EUI64, endpoint, cluster, attribute, ZCL types, network address, interview,
bindings, raw diagnostics, deferred leave, esp-zigbee-sdk.
Наружу Zigbee отдаёт только adapter-contract.

## Что сейчас протекает наружу (убрать)

- `ha_model/ha_entities.h`: `HA_ENTITY_ZB_DIAG_DEVICE`, `HA_ENTITY_ZB_DIAG_STATE`,
  `HA_ENTITY_ENDPOINT`, `ha_device_uid_t`, `ha_zb_state_key_t`, `ha_endpoint_record_t`,
  `ha_device_record_t`, `ha_device_remove_record_t`.
- `ha_model/ha_commands.h`: `HA_CMD_ZIGBEE_CLUSTER`, `ha_zb_command_t` (+ `HA_CMD_DEVICE_REMOVE`).
- `ha_model` включает `ha_model/ha_zigbee.h`.
- `app_main.c`: Zigbee raw-таблицы (`device_desc`, `endpoint_desc`, `device_remove_desc`,
  `ZB_DIAG_*`).
- `web`: `DIAG_ZB_DEVICE/ENDPOINT/STATE`, raw `uid/cluster/attr/zcl_type`.
- `web-ui`: `zcl.js`, raw Zigbee schema, `Diagnostics.jsx` знает EUI64/endpoints/clusters.

## grep-инвариант (после Step C)

Вне `zigbee/`, `zigbee/tests/` и `semantics/` (production generic core) → **0**
идентификаторов:

```
device_uid / EUI64 / endpoint / cluster_id / attr_id / zcl_type
ha_zb_ / HA_ZB_ / HA_ENTITY_ZB_ / HA_CMD_ZIGBEE_
ha_device_record_t / ha_endpoint_* / ha_cluster_entry_t
```

Упоминания слов в комментариях-пояснениях («здесь этого нет») не считаются. host-тесты
generic core (`domain/tests`) используют собственные opaque-структуры с полем `endpoint`
как произвольный layout — это не Zigbee-протечка. Исключения фиксируются явно.

## Порядок (C0…C8)

```
C0  этот документ + фиксация решений
C1  убрать старый Web Zigbee-control path; Diagnostics — read-only
C2  убрать raw Zigbee команды из ha_model (HA_CMD_ZIGBEE_CLUSTER, ha_zb_command_t → private Zigbee)
C3  убрать Zigbee diagnostics entities из Domain (ZB_DIAG_DEVICE/STATE, ENDPOINT)
C4  перенести Zigbee diagnostics state внутрь adapter (private) + Diagnostics API
C5  убрать ha_zigbee.h из ha_model (перенести типы в zigbee/include)
C6  убрать HA_ENTITY_DEVICE_REMOVE из generic Domain (deferred leave → private adapter state)
C7  Web Diagnostics подключить к adapter diagnostics API (не к Domain raw)
C8  финальный grep/boundary review
```

Каждая фаза — чекпоинт (сборка + host-тесты + при необходимости P4 + коммит).

## Открытые решения

1. **Diagnostics (C4/C7).** Целевое: Zigbee adapter владеет raw diagnostics и отдаёт его через
   отдельный diagnostics-интерфейс; Web его запрашивает. Варианты: (a) сразу adapter diagnostics
   API; (b) staged — временно оставить `DIAG_ZB_*` канал, но убрать raw из generic Domain.
   Рекомендую (a) как конечное состояние, реализуя поэтапно (C3/C4/C7).
2. **System adapter.** System device сейчас пишет raw `HA_ENTITY_ENDPOINT`/`HA_ENTITY_STATE`
   (и canonical PROPERTY_STATE). Под C3 ENDPOINT исчезает из Domain → System тоже перестаёт
   писать raw ENDPOINT/STATE, оставляя только canonical PROPERTY_STATE (System — config/misc
   adapter, не Device-model).
3. **`ha_zigbee.h` (C5)** — `ha_device_uid_t`/ZCL-константы переезжают в `zigbee/include/zigbee/`;
   generic core на них не ссылается.
4. **Deferred leave (C6)** — Zigbee-private persistence (напр. маленький adapter store/NVS),
   не generic `HA_ENTITY_DEVICE_REMOVE`.
5. **Порядок C1..C8** подтверждается; реальный removal/re-pair smoke — после C8.

## Не входит в Step C

- Изменение canonical Device/Entity/Property/Event модели (стабильна с Step A/B).
- Matter/GPIO адаптеры (только закладывается их равенство Zigbee как адаптеров).
