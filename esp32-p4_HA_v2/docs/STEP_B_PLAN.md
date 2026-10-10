# Step B — Logical Device Model (normalized Device graph)

Недостающий уровень **над** Entity. Step A разрезал physical Zigbee endpoint на независимые
логические Entity, но потерял структурную принадлежность. Step B возвращает её как
**нормализованный Device graph**, который adapter предоставляет Domain.

## Принцип

> **Adapter owns physical representation; Device service owns normalization into the canonical
> Device graph; Domain only stores that graph.**

И уточнение к Step A: physical **addressing** остаётся adapter-private; stable **external
identity** может храниться в Domain как opaque `native_id` + `source`.

## Две связи (не смешивать)

```text
ownership (Domain, доступна потребителям):
    Logical Device -> Entity            (Entity.device_id)
    Logical Device -> Logical Device    (parent_device_id, дерево)

physical addressing (adapter-private, наружу НЕ выходит):
    Entity <=> EUI64 + endpoint          (горячий путь команды)
```

`native_id` — provenance identity, Domain её **не парсит** (не EUI64/endpoint/ZCL).

## Слои

```text
Zigbee adapter
   ↓ normalized ha_device_desc_t (source, native_id, parent_native_id, name)
generic Device service (над Domain, транспорт-agnostic)
   ↓ derive device_id; upsert Logical Device; bind Entity ownership
Domain
   { device_id, source, native_id, parent_device_id, name }
   Entity { entity_id, device_id, name }
```

Domain **не вызывает adapter**. Удаление инициирует UI → Device service → adapter dispatcher.

## Модель (ha_model — только формы/derivation)

```c
typedef enum {
    HA_SOURCE_NONE   = 0,
    HA_SOURCE_ZIGBEE = 1,
    HA_SOURCE_GPIO   = 2,
} ha_source_id_t;

#define HA_NATIVE_ID_MAX 32

typedef uint64_t ha_device_id_t;
#define HA_DEVICE_ID_NONE ((ha_device_id_t)0u)

typedef struct { ha_device_id_t id; } ha_logical_device_key_t;

typedef struct {
    ha_source_id_t  source;
    ha_device_id_t  parent_device_id;      /* HA_DEVICE_ID_NONE — root */
    char            native_id[HA_NATIVE_ID_MAX];
    char            name[HA_DEVICE_NAME_MAX];
} ha_logical_device_record_t;

/* DTO, который adapter подаёт в Device service (adapter описывает внешний граф). */
typedef struct {
    ha_source_id_t source;
    char           native_id[HA_NATIVE_ID_MAX];        /* канонизирует adapter */
    char           parent_native_id[HA_NATIVE_ID_MAX]; /* "" == root */
    char           name[HA_DEVICE_NAME_MAX];
} ha_device_desc_t;

/* Единственная derivation identity (никаких альтернативных формул в adapter). */
ha_device_id_t ha_device_id_derive(ha_source_id_t source, const char *native_id);
```

`ha_entity_record_t` (Step A/B2) = `{ ha_device_id_t device_id; char name[...]; }` — без изменений.

## Derivation (единая)

```text
device_id        = ha_device_id_derive(source, native_id)
parent_device_id = ha_device_id_derive(source, parent_native_id)   (если parent_native_id != "")
```

`ha_device_id_derive` — generic в `ha_model` (SipHash, тот же примитив, что `entity_id`).
Adapter **не** имеет своей формулы: он канонизирует `native_id` и зовёт эту функцию.

## native_id

Opaque строка, канонизацией владеет adapter. Требования: стабильна, уникальна в пределах
`source`, без пробелов. Для Zigbee: lowercase hex EUI64, 16 символов, без `0x`/`:`.

## Device service (generic, над Domain; НЕ в ha_model)

`device_register(domain, desc)`:
```text
native_id        → derive → device_id
parent_native_id → derive → parent_device_id (или NONE)
ensure Logical Device { source, parent_device_id, native_id, name }   (имя не перетираем)
```
`device_bind_entity(domain, device_id, entity_id)` — выставить Entity.device_id (ownership).
`device_remove` — прочитать Logical Device, вызвать adapter dispatcher по `(source, native_id)`,
после успеха — снять ownership с Entity и удалить Logical Device.

Domain остаётся storage/runtime; решения о нормализации — в Device service.

## Removal (порядок зависимостей)

```text
UI -> Device service -> read Logical Device (Domain) -> (source, native_id)
   -> adapter dispatcher (unpair физического объекта)
   -> успех -> Device service: снять Entity.device_id (iter+filter) + удалить Logical Device
```

Поиск Entity конкретного Device — `iter()+filter(device_id)`; mstore не расширяем.

## Deletion semantics

- Remove Entity — только Entity; физическое устройство не трогается.
- Remove Logical Device — adapter-specific removal (unpair), затем чистка Entity + Device.

## Non-goals (сейчас)

- Искусственная разбивка endpoint → child Device (child только когда adapter реально обнаружит).
- Device merge/split/reassign.
- Generic query/index API в mstore (поиск — scan/iter).

## Migration

Logical Device и Entity — **RAM-таблицы** (восстановимы из интервью), поэтому смена ABI записи
не требует flash-erase. Device-id значения не персистятся.

## Порядок

```text
B1'  ha_model: source/native_id/parent/name формы + ha_device_id_derive (generic) + тесты
B2'  Entity.device_id (ownership)                                                  [x] (B2)
B3'  generic Device service: register / bind_entity / remove primitives
B4'  Zigbee adapter: интервью строит нормализованный Device graph и подаёт в Device service;
     entity binding (entity_id ↔ EUI64+endpoint) остаётся private
B5   WS v3 + UI: Device → Entity grouping (дерево по parent)
B6   rename Logical Device (в Domain; reverse device binding не нужен)
B7   removal lifecycle + adapter dispatcher
B8   старый uid-keyed HA_ENTITY_DEVICE → HA_ENTITY_ZB_DIAG_DEVICE + cleanup
B9   boundary review + smoke на P4
```

## Зафиксированные решения

1. `parent_device_id` вводим **сейчас** (root = NONE); adapter заполняет NONE для плоских.
2. Derivation — **одна generic** `ha_device_id_derive` (в `ha_model`), без формул в adapter.
3. `native_id` — opaque string, канонизация adapter-side, `HA_NATIVE_ID_MAX = 32`.
4. Формы и derivation — `ha_model`; lifecycle (register/bind/remove) — отдельный generic
   Device service над Domain.
5. Domain не вызывает adapter (зависимость: UI → service → Domain; adapter — через service).

## Известная коллизия имён

`ha_endpoint_record_t.device_id` / `zigbee_endpoint_desc_t.device_id` — это **ZCL device id**
(напр. `0x0302`), не `ha_device_id_t`. При работе с Device graph не путать (переименование —
B8/позже).
