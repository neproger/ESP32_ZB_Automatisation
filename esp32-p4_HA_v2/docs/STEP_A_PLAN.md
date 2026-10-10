# Шаг A — canonical semantic identity: аудит и план

> Baseline: ветка `property-model-migration`, коммит `da30b93` (Фаза 5 закрыта).
> Контракт семантики — `PROPERTY_MODEL.md`; журнал миграции — `PROPERTY_MODEL_IMPL_JOURNAL.md`.
> Это **план**, а не код. Шаг A — необратимый (erase) класс изменений; сначала аудит.

## 1. Baseline (что зафиксировано перед A)

```text
Domain:       canonical пока physical (uid, ep, cluster, attr); API агностичен
Semantics:    Property/Action/Event — единственный мост Zigbee ↔ семантика
Web:          semantic BFF поверх raw WS v2 (аддитивные message types)
UI:           полностью семантический (cluster/attr/ZCL — только Diagnostics)
Storage:      raw ABI сохранён; semantic живёт как проекция в рантайме
```

Проверено: обычные pages/components не интерпретируют ZCL (grep-чек 5.5.7). Baseline рабочий
end-to-end (state/command/automation/event/group/capabilities).

## 2. Аудит по восьми направлениям

### 2.1. Semantics API — есть протечки транспортных понятий
- `ha_properties.h`: `PROPERTY_*`, `ACTION_*`, `EVENT_*`, `ha_value_t`, `ha_unit_t`,
  `ha_capability_t` — **транспорт-агностичны** ✓.
- **Протечка:** `ha_event_t` (`device_uid` u64 + `endpoint` u8) и семантическая модель правила
  (`ha_sem_ref_t`, `ha_sem_trigger_t`, `ha_sem_condition_t`, `ha_sem_action_t` в
  `ha_automation.h`) используют `device_uid` (EUI-флейвор) и `endpoint` (понятие Zigbee).
  Для GPIO/Matter `endpoint` бессмысленен. В A заменить на `entity_id` (opaque,
  adapter-owned); `property/action/event` сохраняются.
- `semantics.c` (ZB_MAP/ZB_ACTION_MAP/ZB_EVENT_MAP/ZB_CAPABILITY_MAP) — Zigbee-private ✓.
- Вывод: vocabulary чист, **адресация — нет** (нужен `entity_id`).

### 2.2. Domain ownership и identity
- Domain агностичен (типы/команды/ключи — opaque байты). ✓
- Сейчас identity = `device_uid` (EUI-64) + endpoint + cluster + attr. EUI стабилен
  после reboot и re-pair.
- Нет явного `Entity`. В A нужен: **кто** создаёт entity_id (рекомендация — Zigbee bridge при
  интервью), **стабильность** (детерминированно из персистентного binding, независимо от
  порядка обнаружения), **владение** (adapter создаёт binding; Domain владеет canonical
  (entity_id, property_id)).

### 2.3. Transitional места (таблица «удаляется → заменяется»)

| Transitional (raw) | Замена (semantic) | Шаг A |
|---|---|---|
| `ENTITY STATE` (cluster/attr) | semantic state `(entity, property)` | A1/A2 |
| `WEB_CMD_ZB_COMMAND` (2) | `WEB_CMD_SEMANTIC_COMMAND` (14) | A8 |
| `ENTITY AUTOMATION` + `WEB_CMD_AUTOMATION_PUT` (4) | persistent semantic rule | A3 |
| `ENTITY GROUP_ITEM` + `GROUP_ITEM_PUT/REMOVE` (11/12) | persistent semantic group item | A4 |
| event shim: raw `ha_zb_event_t` в payload | rule хранит `event_id` | A5 |
| `schema.js` raw decoders, `zcl.js` | Diagnostics-only | A8 |
| physical `ha_automation_record_t` (144) | semantic record | A3 |
| raw `group_item` key (uid,ep,cluster,attr) | `(group, entity, property)` | A4 |

### 2.4. Persistence migration
- FLASH-таблицы: device, endpoint, device_remove, location, group, group_item, automation,
  settings (+ wifi_known). `mstore` сверяет `payload_size` в заголовке
  (`mstore_storage_flash.c:127`) → смена формы/размера → `INVALID_STATE` → **erase**.
- A меняет: state key (не персистится — RAM), `automation` record (FLASH), `group_item` key
  (FLASH), возможно `endpoint`/`device`.
- **Решение (рекомендация):** принять разовый erase затронутых FLASH-таблиц. Безопаснее и
  проще, чем миграция. Единожды теряются: правила, экраны/виджеты, location, settings
  (location/settings восстанавливаются; правила/экраны — руками). Механизм: смена
  `persist_key` (напр. `automation`→`automation_a`) или явное стирание региона.

### 2.5. Failure semantics (текущее + целевое)
- неизвестный property → состояние не публикуется (semantic), условие=false, событие не
  маппится, group item не публикуется. Честная деградация, не выдумываем смысл.
- удалённый endpoint/устройство → `zigbee_device_remove` снимает DEVICE/ENDPOINT/STATE.
- re-interview → топология переприменяется; entity_id обязан остаться тем же.
- старое правило (raw) → `representable=0` в semantic stream (legacy/read-only).

### 2.6. Memory/CPU
- Сейчас в Domain **один** state store (raw ZCL); семантика — проекция на лету (второй копии
  нет). ✓
- Правило A: canonical = semantic store (truth); physical (cluster/attr) — **adapter metadata**
  (binding), не второй state. Не допускать двух постоянных копий состояния.

### 2.7. Snapshot/recovery
- state — RAM, восстанавливается из репортов после интервью. Персистятся device/endpoint/
  automation/groups/location/settings.
- A: `entity_id` должен быть **детерминирован** из персистентного binding, чтобы reboot (в
  любом порядке обнаружения) дал ту же identity.

### 2.8. Binary protocol v3
- Перед v3 определить **финальную минимальную форму**, не переносить временные DTO 1:1.
- v3 несёт только семантику: entity (device/entity/property + value), capabilities,
  automation-rule (semantic), group (semantic), event (source/event_id/value), command
  (property/action/value). Raw physical кадры из обычного провода убираются
  (Diagnostics — либо без провода, либо отдельный raw-канал).

## 3. Целевая модель A

**Identity и binding разделены.** Domain знает только `(entity_id, property_id)`; транспорт
приходит через binding (adapter-owned metadata).

```text
Entity
  entity_id        (opaque uint64; стабильная logical identity)
  kind / metadata

Binding
  entity_id
  transport          (ZIGBEE | GPIO | MATTER | …)
  transport_address   — Zigbee: { device_uid, endpoint }
                        GPIO:   { gpio_num }
                        Matter: { node_id, endpoint }

Property
  id = POWER         (транспорт-агностично)

canonical state key: (entity_id, property_id)
```

`entity_id`: **opaque `uint64`, детерминированно выводится из persistent binding**
(например derivation/hash от `transport + transport_address + logical_slot`); наружу
структура не раскрывается. **Не** `entity_id = uid` и **не** упаковка `uid+endpoint` — иначе
идентификатор снова становится Zigbee-centric.

**Обязательное правило A0:**

> `entity_id` должен быть восстановим из binding **без зависимости от порядка discovery** и
> **без отдельного mutable counter**. Иначе после erase/reboot/re-interview ID начнут плавать.

Правило A: **Domain больше не требует cluster/attr для хранения состояния.** Если generic
Domain API где-то требует cluster/attr — миграция не завершена.

## 4. Фазы A (без «большого взрыва»)

```text
A0  logical entity identity + binding model (canonical state пока не меняем)
    A0.0  контракт entity_id и binding
    A0.1  ha_entity_id_t + transport-agnostic Entity (kind/metadata)
    A0.2  Zigbee binding record { entity_id, transport=ZIGBEE, uid, endpoint }
    A0.3  детерминированная derivation entity_id из binding (алгоритм — §5)
    A0.4  Zigbee bridge создаёт/восстанавливает Entity при интервью
    A0.5  tests: reboot / порядок discovery / re-interview → та же identity

A1  canonical state (entity_id, property_id)
      Новый тип canonical state; physical ZCL key уходит в adapter binding.
      Domain API по-прежнему агностичен (opaque key).

A2  Zigbee bridge пишет/читает canonical state
      Репорт → semantics → canonical (entity, property). Mapping cluster/attr ↔ property
      полностью внутри bridge. Consumers читают (entity, property).

A3  Automation persistent semantic
      Правило хранит (entity, property, op/value, action...) вместо 144-байтной physical.
      entities/группы правил — erase/migrate.

A4  Group Item persistent semantic
      Ключ (group, entity, property) вместо raw (uid,ep,cluster,attr).

A5  event shim removal
      Убрать raw ha_zb_event_t из payload; rule хранит event_id.

A6  WS v3
      Финальная минимальная семантическая форма; raw physical кадры убраны.

A7  erase старого physical state ABI
      Удалить raw state store/таблицы; освободить регионы.

A8  удалить transitional Web/Domain paths
      WEB_CMD_ZB_COMMAND, raw AUTOMATION/GROUP_ITEM, schema raw decoders (кроме Diagnostics),
      per-save compile/decompile.
```

Каждая фаза заканчивается чекпоинтом (сборка + host-тесты + проверка на P4 + коммит).
До A7 хранение ещё позволяет откат; A7 — точка невозврата.

## 5. Решения (приняты) и один открытый пункт

Принято:

1. **entity_id** — opaque `uint64`, детерминированно из persistent binding
   (`transport + transport_address + logical_slot`); не `uid` и не packing `uid+endpoint`
   (§3). Восстановим из binding, без mutable counter, без зависимости от порядка discovery.
2. **Erase vs migration** — принять **разовый erase** несовместимого persistent state
   (`automation`/`group`/`state`). Миграция physical→semantic не оправдана. Совместимые
   `settings`/`location` сохранить, только если их таблицы не меняются; иначе тоже erase
   (восстановимы).
3. **WS v3** — **новый минимальный canonical protocol**, не продолжение v2. Frame types:
   `ENTITY`, `PROPERTY_STATE`, `CAPABILITIES`, `AUTOMATION`, `GROUP_ITEM`, `EVENT`; команды:
   `PROPERTY_ACTION`, `AUTOMATION_PUT`, `GROUP_ITEM_PUT`/`REMOVE`. Без `cluster`/`attr`/
   `zcl_type`/raw automation record.
4. **Diagnostics** — отдельный **transport-specific** diagnostic message/канал, **вне**
   canonical v3. Возможность видеть cluster/attr сохраняется, основной контракт остаётся чистым.
5. **System** — обычная logical entity `ENTITY_SYSTEM` со свойствами `SYSTEM_TIME_*`.
   **Weather** — отдельная entity `ENTITY_WEATHER` с semantic properties. **Location/settings** —
   отдельные config entities (не live property state), в Property-модель не тащить.

Открыто (решить в **A0.3**, аккуратно и один раз):

- **Точный алгоритм derivation `entity_id`** — конкретная функция от binding. Требования:
  детерминизм, устойчивость к порядку discovery, отсутствие коллизий, стабильность при re-pair
  (EUI стабилен). Кандидаты: `hash64(transport, uid, endpoint, logical_slot)` с фиксированным
  seed; либо персистентный id, выданный один раз и хранимый в binding (но тогда он должен
  переживать erase/reboot — противоречит «восстановим из binding»). Предпочтение — чистая
  детерминированная derivation без хранения отдельного id.
