# Step B — Logical Device Model

Недостающий уровень **над** Entity. Step A разрезал physical Zigbee endpoint на независимые
логические Entity, но потерял **структурную принадлежность**: какие Entity — части одного
реального устройства. Step B возвращает эту связь, не ломая Step A.

Это не откат от Entity-модели, а слой владения над ней.

## Две разные связи (не смешивать)

```text
ownership (Domain, доступна потребителям):
    Logical Device -> Entity

transport binding (adapter-private, наружу НЕ выходит):
    Zigbee Device <=> EUI64
    Entity        <=> EUI64 + endpoint
```

Domain знает только `device_id`, `entity_id`, `property_id`. `EUI64`/`endpoint` живут строго
в adapter binding.

## Инварианты

```text
Logical Device != transport device record.
Logical Device — для ownership / lifecycle / UI grouping.
Entity остаётся единицей properties / actions / events.

Deleting Entity  не означает удаление физического Zigbee-устройства.
Deleting Logical Device может инициировать adapter-specific device removal (unpair).
```

Дополнительно:
- Domain не знает `EUI64`/`endpoint`/`cluster`/`attr`.
- `device_id` — opaque `uint64`, детерминирован, Domain его не выводит.
- Потребители обычного UI работают от `Device -> Entity`, не от Zigbee.

## Модель (минимальный этап — без дерева)

```c
typedef uint64_t ha_device_id_t;
#define HA_DEVICE_ID_NONE ((ha_device_id_t)0u)

/* Logical Device: ключ — device_id (8). */
typedef struct { ha_device_id_t id; } ha_device_key_t;
typedef struct { char name[HA_DEVICE_NAME_MAX]; } ha_logical_device_record_t;

/* Entity получает логическую связь (Step B): */
typedef struct {
    ha_device_id_t device_id; /* HA_DEVICE_ID_NONE — не привязана (edge) */
    char name[HA_ENTITY_NAME_MAX];
} ha_entity_record_t;
```

`HA_ENTITY_NAME_MAX` — текущий 32. `HA_DEVICE_NAME_MAX` — 32 (совпадает с именем устройства
адаптера), вынести явно.

## Identity / derivation

`device_id` — детерминированный, из EUI64, тем же примитивом, что и `entity_id` (Step A0.3):

```text
device_id = ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, LE(EUI64), 8)
entity_id = ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, LE(EUI64) || endpoint, 9)
```

Разная длина seed (8 vs 9) → `device_id != entity_id` для того же устройства; reboot/order-
independent; persistence не нужен. Выставляется **только** adapter'ом.

## Владение (ownership) vs binding

- Adapter при интервью: для `EUI64` создаёт/находит Logical Device (`device_id`), задаёт
  initial name (== имя physical device, как сейчас), затем для каждого endpoint — Entity с
  `device_id`.
- Adapter-private registry: `device_id <=> EUI64`, `entity_id <=> EUI64 + endpoint`.
- Domain: `Logical Device(id, name)` + `Entity(id, device_id, name)`.

## Deletion semantics

- **Remove Logical Device** (UI) → adapter получает запрос на unpair физического EUI64; после
  leave — удаляются связанные Entity (по `device_id`) и сам Logical Device.
- **Remove Entity** (если такое появится) — удаляет только Entity; физическое устройство НЕ
  трогается.
- Поиск Entity конкретного Device — обычный `domain_entity_iter(ENTITY) + filter(device_id)`.
  **mstore не расширяем** (никаких generic query/index API ради Step B).

## Migration / ABI

`ha_entity_record_t` меняет раскладку (добавляется `device_id`) → несовместимый persistent
ABI. Разовый controlled erase (как A7): Logical Device и `device_id` в Entity восстановимы из
интервью после cold boot, поэтому миграция не нужна.

## Non-goals (сейчас НЕ делаем)

- `parent_device_id` / дерево устройств.
- Device merge / reassign / split.
- Generic device/entity query API в mstore.
- Возврат физических полей (`EUI64`) в Domain.

## Future (описать, поле сейчас НЕ добавлять)

`parent_device_id` и составные устройства (Device, содержащий Device'ы). Добавляем только
когда первый реальный adapter покажет device-in-device. До этого поле в структуру не вводим —
иначе это преждевременный публичный ABI под дерево.

## Порядок

```text
B0  docs + model contracts (этот документ) + фиксация решений
B1  ha_device_id_t + модель Logical Device (ha_model, пассивная форма) + тесты
B2  Entity.device_id + смена ABI (mstore erase), обновить формы/ключи
B3  Zigbee adapter: derivation device_id + device binding (device_id <=> EUI64) + тесты
B4  интервью создаёт/обновляет Logical Device и связывает Entity (device_id)
B5  WS v3 + UI: grouping Device -> Entities
B6  rename: Logical Device rename (уже есть ENTITY_RENAME для Entity; добавить DEVICE_RENAME v3)
B7  lifecycle: Remove Logical Device -> adapter unpair -> clean Entity + Device
B8  переименовать старый uid-keyed HA_ENTITY_DEVICE -> HA_ENTITY_ZB_DIAG_DEVICE
    (diagnostics-only, по аналогии с ZB_DIAG_STATE) + cleanup комментариев/веба
B9  review: инварианты, grep (Domain не знает EUI64), тесты, smoke на P4
```

Каждая фаза — чекпоинт (сборка + host-тесты + при необходимости P4 + коммит). Реализацию
начинать с B1 после подтверждения этого плана.

## Открытые вопросы

- Имя нового доменного типа: `HA_ENTITY_LOGICAL_DEVICE` (предложение) — ок?
- Переименование старого `HA_ENTITY_DEVICE` → `HA_ENTITY_ZB_DIAG_DEVICE` делать сразу в B8
  (после того как Logical Device заработает), чтобы не путать два «device».
