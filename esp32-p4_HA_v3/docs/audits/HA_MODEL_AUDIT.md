# Аудит `ha-model`: canonical types / ABI

> Статус: **аудит завершён для ABI структур и идентификаторов; property descriptors —
> открытая часть**. Код и план не менялись.
>
> Эталон V2: `esp32-p4_HA_v2/ha_model/include/ha_model/*.h`, `ha_model/src/*.c`,
> `docs/RECORD_MODEL.md`, `docs/services/WEB_PROTOCOL.md`.
> Реализация V3: `crates/ha-model/src/{lib.rs,canonical.rs}` и потребители.
>
> Принцип: отделять **факт V2** (что было реализовано и проверено), **решение V3**
> (что сохраняем) и **Rust-исключение** (что допустимо изменить). Устаревшие
> V2-тесты не являются эталоном layout'а.

## 1. V2 contract (по исходникам)

- `ha_entities.h`: 13 типов сущностей, числа `4,6..18`, дырки `1,2,3,5` —
  стабильный ABI, значения не переиспользуются.
- `ha_properties.h`: `ha_value_t` (8 байт: `kind` enum`@0`, `value` union`@4`),
  property/action/event id 0..22 / 0..4 / 0..8, `ha_property_desc_t` +
  `ha_property_desc()` (kind, unit, range, flags).
- `ha_entity.h`: key 8, record 40 (`device_id@0`, `name[32]@8`); SipHash-2-4 с
  фиксированным ключом над `LE32(issuer)||LE32(len)||seed`, seed ≤ 64.
- `ha_logical_device.h`: key 8, record 80 (`parent@0`, `source@8`, `native_id@12`,
  `name@44`, `reserved@76`).
- `ha_state.h`: key 16 (`entity@0`, `property@8`, `reserved@12`), record 8.
- `ha_capabilities.h`: `ha_capability_t` 24 (`property@0`, `action_count@4`,
  `actions@8`); record 216 (`event_count@196`, `events@200`).
- `ha_commands.h`: `ha_property_command_t` 32 (`entity@0`, `property@8`, `action u8@12`,
  `reserved@13`, `ha_command_value_t@16`, tail`@28`), `ha_entity_rename_t` 40,
  cmd ids 3/4/5/6/7.
- `ha_settings.h` 8; `ha_weather.h` 16; `ha_wifi.h` scan 580 / known 96 / status 36;
  `ha_groups.h` group 32, group_item 36 (`order@0`, `reserved@2`, `title@4`),
  ключи 8/16.
- `ha_rule.h`: `ha_rule_t` **456** (`triggers@0`, `actions@128`, `conditions@256`,
  `enabled@384`, `trigger_count@385`, `action_count@386`, `conditions_count@387`,
  `name[64]@392`); вложенные trigger/action/condition 32, ref 16, compare 12.
- `ha_event_t`: 24 (`id@0`, padding`@4`, `source_entity@8`, `ha_value_t`@16).
- `ha_automation.h`: trigger kind 0..2, edge 0..2, condition op 1..8.
- `ha_entity.c`: SipHash key `"ESP32-P4-HA-v2!" + NUL`, `entity_id_derive`,
  `ha_entity_singleton`; Zigbee issuer = 1 (`zigbee_adapter_ids.h`), system = 2.
- `WEB_PROTOCOL.md`: entity types и `ha_property_command_t`/`ha_event_t` передаются
  verbatim; таблица команд 1,4,5,8,9,10,13,16,17,18,19,20,21,22,23.

### 1.1. Источник истины для `ha_rule_t`

`ha_model/tests/test_rule.c:13-16` всё ещё требует `sizeof(ha_rule_t) == 392` и не
знает `name`. Это устаревший тест: актуальный заголовок `ha_rule.h` требует 456,
`name@392`, а `app_main.c` использует `persist_key = "automation_v5"` с комментарием
`+ name[64]`, и `WEB_PROTOCOL.md` указывает 456.

**Правило:** эталон — заголовок V2 и `WEB_PROTOCOL.md`; значения для сверки берутся
из них, а не из V2-теста. Тест в V3 обязан независимо проверять размер и офсеты,
включая `enabled@384`, `action_count@386`, `name@392`.

## 2. V3 implementation

`crates/ha-model` содержит идентификаторы, `Value`, и структуры/кодеки для Entity,
LogicalDevice, State, Capabilities, Location, Weather, Wifi×3, Settings, Command.
Тесты `canonical.rs` подтверждают SipHash-вектор, golden `zigbee_entity_id`,
размеры, round-trip команд, layout location/weather.

## 3. Differences (факты)

| # | Тип | V2 | V3 | Статус |
|---|---|---|---|---|
| 1 | Entity type ids | 4,6..18 | те же | ✅ |
| 2 | Property/action/event ids | 0..22 / 0..4 / 0..8 | значения 1..22 / 1..4 / 1..8 | ✅ значения; ⚠ нет констант `UNKNOWN`/`NONE` |
| 3 | `ha_value_t` 8 | kind enum`@0`, value`@4` | kind u8 + 3 нуля + bits`@4` | ✅ на LE; нет теста `bytes[1..4]==0` |
| 4 | Entity key/record 8/40 | ✅ | ✅ | ✅ |
| 5 | LogicalDevice 80 | ✅ | ✅ | ✅ |
| 6 | State key/record 16/8 | ✅ | ✅ | ✅ |
| 7 | Capability / Capabilities 24/216 | ✅ | ✅ | ✅ |
| 8 | Location 60 | ✅ | ✅ | ✅ |
| 9 | Weather 16 | ✅ | ✅ | ✅ |
| 10 | Wifi scan/known/status 580/96/36 | ✅ | ✅ | ✅ |
| 11 | Settings 8 | ✅ | ✅ | ✅ |
| 12 | PropertyCommand 32 / CommandValue 12 | ✅ | ✅ байты (`CanonicalCommand`) | ✅ |
| 13 | EntityRename 40 | ✅ | отсутствует | ❌ |
| 14 | Event 24 | ✅ | только encoder в `ha-app/web.rs:187` | ❌ |
| 15 | `ha_rule` 456 + вложенные + enums | ✅ | `ha-automation`: локальные 456 и офсеты 384–387 | ❌ дубль |
| 16 | `ha_group` / `ha_group_item` | ✅ | только `*_LEN`; layout парсит `display.rs:637,663` | ❌ |
| 17 | `ha_property_desc` (kind/unit/range/flags) | ✅ | отсутствует | ❌ открытая часть |
| 18 | Wifi auth/state enums | ✅ | отсутствуют (raw u8) | ⚠ |
| 19 | Domain cmd ids 3/7 | `DISCOVERY`, `ENTITY_RENAME` | нет в `ha-model` | ⚠ |
| 20 | Web type 12 (WIFI_STATUS) | в протоколе | нет в `ha_web::entity` | ⚠ (Web-слой) |
| 21 | Singleton entity id | derived | hardcode `0x9229…`, нет golden-теста | ⚠ |
| 22 | Device DTO 100 | `ha_device_desc_t` | нет (adapter-private) | Rust-исключение, приемлемо |

### 3.1. Офсеты `ha_rule` в потребителях

`ha-automation/src/lib.rs` читает `enabled@384`, `trigger_count@385`,
`action_count@386`, `conditions_count@387`; триггеры/действия/условия — по
смещениям `base+0..30`. По чтению V2-заголовка значения совпадают, но это пока
**не доказано тестом** и не должно считаться совместимостью до его появления.
`main.rs:216` пишет `record[384]`, `display.rs:637,663` разбирает
`group`/`group_item` по magic-офсетам.

## 4. Required behavior

1. `ha-model` — единственный источник id, длин и байтовых раскладок; сервисы не
   содержат magic-офсетов.
2. Перенесённые раскладки фиксируются compile-time assert'ами (размеры и офсеты)
   и golden-байтами.
3. V3 не опирается на устаревшие V2-тесты: эталон — заголовки и `WEB_PROTOCOL.md`.
4. `persist_key` версии (`automation_v5`, `group_item_v3`) остаются привязанными к
   layout'у.
5. Rust-исключения (отсутствие `repr(C)`, ручные кодеки, adapter-private DTO)
   допустимы, но байты обязаны совпадать.

## 5. Stage mapping

- **Этап 1** — дополнить canonical ABI:
  - `EntityRename` 40, `Event` 24, `Rule` 456 (+ вложенные + enums),
    `Group` 32 / `GroupItem` 36 / `GroupAutomationKey` 16;
  - `PropertyDesc` + таблица (структуры; построчное сравнение — открыто, см. §7);
  - `WifiAuth` / `WifiState`;
  - недостающие константы: `PROPERTY_UNKNOWN`, `ACTION_NONE`, `EVENT_NONE`,
    `SOURCE_NONE`, `COMMAND_DISCOVERY`, `COMMAND_ENTITY_RENAME`;
  - compile-time assert'ы размеров/офсетов; тест `desc == canonical`;
    тест `V2↔V3 layout` (456 / 384 / 386 / 392); golden singleton.
- **Этап 3** — переключить потребителей: `ha-automation` (удалить
  `RULE_RECORD_LEN`, использовать canonical-раскладку), `main.rs` (`record[384]`),
  `display` (group/group_item), `ha-app/web.rs` (event encoder → `ha_model::Event`).
- **Этап 6** — wire parity: Web snapshot/delta, Zigbee-проекция и storage несут
  байт-идентичные записи.

## 6. Tests

- `canonical_layout_v2_parity`: размеры и офсеты всех раскладок против V2-заголовков,
  включая `ha_rule_t` 456, `enabled@384`, `action_count@386`, `name@392`.
- `value_abi_is_4byte_kind`: `bytes[0]=kind`, `bytes[1..4]=0`, bits LE`@4`.
- `desc_geometry_matches_canonical`: для каждой сущности `key_size`/`record_size`
  descriptor'а равны `canonical::*_KEY_LEN` / `*_RECORD_LEN`.
- `entity_singleton_matches_v2`: golden для `ha_entity_singleton(issuer)`.
- `event_layout_matches_v2`: 24 байта, `id@0`, `source@8`, value`@16`.
- **Отрицательный тест API (compile-fail)**: локальный `RULE_RECORD_LEN` в
  `ha-automation` не существует, а сервисный layout не используется вместо
  canonical-структур. Реализация — `trybuild` либо проверка отсутствия публичного
  API (например, что `ha_automation` не экспортирует собственный record-тип и
  `RULE_RECORD_LEN`).

## 7. Открытые части аудита

- **Property descriptors.** Построчное сравнение `ha_model/src/ha_properties.c`
  (kind, unit, range, flags, `ha_property_desc`) с V3 не выполнено. После переноса
  структур пункт **не считается закрытым**: нужна сверка всех 22 свойств и
  решения, где живёт словарь (`ha-model`) и как его потребляют Display/Automation
  без дублирования.
- **Web type 12 (WIFI_STATUS)** и покрытие Wi-Fi сущностей в `ha-web` — проверить
  на слое Web.
- **Golden для singleton** — сейчас значение hardcode; подтвердить алгоритмом V2.

## 8. Следующий слой

`micro-db`: сначала восстановить фактический контракт V2 (schema, slot lifecycle,
meta, index, backends, recovery), затем сравнить с V3, зафиксировать расхождения
и распределить исправления по этапам. Без изменений кода и плана до отдельного
согласования.
