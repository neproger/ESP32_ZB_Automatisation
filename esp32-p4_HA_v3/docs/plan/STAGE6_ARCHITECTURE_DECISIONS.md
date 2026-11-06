# Stage 6 — Architecture Decisions: Web + Display

> Статус: **approved decisions; production code не изменён**.
> Audit: [STAGE6_WEB_DISPLAY_AUDIT.md](../audits/STAGE6_WEB_DISPLAY_AUDIT.md).
> Source of truth: V2 `WEB.md`, `WEB_PROTOCOL.md`, `web/src/*`, `display/src/*`.
> Цель: V2 behavioural parity; V3 protocol number не является основанием менять семантику.

## 1. Web: V2 snapshot/model-state parity

Принято:

- `/model.bin` восстанавливается как V2 static MODEL blob.
- `SNAPSHOT_STATE` возвращает только live state-типы.
- Полный `SNAPSHOT` строится из Domain по явной Web SCHEMA.
- Web не публикует типы вне SCHEMA; правило «всё, кроме `WIFI_KNOWN`» удаляется.
- `WIFI_SCAN` и `WIFI_KNOWN` в Web не отдаются.
- Full-snapshot fallback вместо `SNAPSHOT_STATE` не сохраняется; это было бы
  неявным расширением V2-контракта.

### 1.1. Exact Web SCHEMA

`model` = входит в `/model.bin`; `state` = входит в `SNAPSHOT_STATE`.

| Entity | type | key | record | class | Назначение |
|---|---:|---:|---:|---|---|
| `AUTOMATION` | 4 | 8 | 456 (`ha_rule_t`) | model | правила автоматизации |
| `LOCATION` | 6 | 8 | 60 | model | координаты/пояс/имя |
| `GROUP` | 7 | 8 | 32 | model | экран/группа |
| `GROUP_ITEM` | 8 | 16 | 36 | model | плитка Entity на экране |
| `WEATHER` | 9 | 8 | 16 | state | текущая погода |
| `WIFI_STATUS` | 12 | 8 | 36 | state | состояние Wi-Fi |
| `SETTINGS` | 13 | 1 | 8 | model | настройки UI |
| `ENTITY` | 14 | 8 | 40 | model | имя/владелец Entity |
| `PROPERTY_STATE` | 15 | 16 | 8 | state | canonical state |
| `CAPABILITIES` | 16 | 8 | 216 | model | properties/actions/events |
| `LOGICAL_DEVICE` | 17 | 8 | 80 | model | логическое устройство |
| `GROUP_AUTOMATION` | 18 | 16 | 36 | model | плитка автоматизации |

Явно **не входят**: `WIFI_SCAN` (type 10), `WIFI_KNOWN` (type 11).
Размеры должны быть проверены parity-тестом против `ha_model`; таблица не является
основанием для ручных offsets в Web.

### 1.2. `/model.bin`

V2 contract:

```text
header (12 bytes):
  magic[4] = "HASM"
  ver u8 = 1
  flags u8 = 0
  reserved u16 = 0
  record_count u32 LE
payload:
  [type u8][key][record] × record_count
```

- только `model`-типы из SCHEMA;
- little-endian, canonical key/record verbatim;
- сборка из Domain, lazy rebuild при изменении model-типа;
- `Cache-Control: no-store`;
- ошибка памяти/сборки — HTTP 500, не пустой 200;
- `/model.bin` не 404.

### 1.3. Snapshot / state

```text
SNAPSHOT (full):
  SYNC_BEGIN(count = все SCHEMA records)
  UPSERT(type,key,record) × N
  SYNC_END(count = N)

SNAPSHOT_STATE:
  SYNC_BEGIN(count = 0)       // V2 implementation sends 0 here
  UPSERT только PROPERTY_STATE / WEATHER / WIFI_STATUS
  SYNC_END(count = 0)
```

`SNAPSHOT_STATE` не содержит model-записей. Полный snapshot не подменяет
`SNAPSHOT_STATE`.

## 2. Web delivery

- `seq` — отдельный счётчик на каждого клиента, начиная с 0.
- Bounded queue: одновременно **64 кадра и 16 KB**; превышение закрывает сессию.
- `MAX_CLIENTS = 8` как V2.
- `REMOVE` строится из Domain fact и broadcast независимо от какого-либо Web cache.
- Web-owned MODEL cache удаляется: snapshot, delta и REMOVE не зависят от cache.
- WebSocket send выполняется вне внутреннего mutex; очередь защищается mutex,
  `httpd_ws_send_frame_async` — нет.
- Медленный клиент закрывается; после reconnect получает свежий snapshot.

## 3. Display

- `display::start(domain)` (или эквивалент) сам выполняет initial Domain read:
  подписка → чтение settings/status → начальная сборка. `main` не обязан вызывать
  скрытый `rebuild` после `start`.
- Сохранить task/inbox: Display не блокирует Domain и не владеет canonical state.
- Команды Display идут через согласованный Domain/Device path, как V2:
  `PROPERTY_COMMAND` → Domain; automation enable → canonical rule update source UI;
  Wi-Fi scan/connect → Domain commands. `intake` не должен быть скрытым обязательным
  посредником для Display business path.
- Ручные offsets (`bytes[0]`, `4 + 24*i`, `8..40` и т.п.) заменить декодерами
  `ha_model` (`CapabilitiesRecord`, `EntityRecord`, `LocationRecord`, `WeatherRecord`,
  `SettingsRecord`, `WifiStatusRecord`, `WifiScanRecord`).
- Targeted updates по V2:
  - property state — только соответствующий widget;
  - `AUTOMATION`/`GROUP_AUTOMATION` — switch/плитка без полного rebuild;
  - `WIFI_SCAN`/`WIFI_STATUS` — status/list без пересборки всей Wi-Fi страницы.
- Rebuild только для состава/имени/capabilities, как V2.
- Удалить `#![allow(warnings, clippy::all)]` после исправления конкретных warnings;
  не маскировать диагностику.
- Wi-Fi данные остаются только через Domain; прямые C radio calls запрещены.

## 4. Parity tests before implementation

- initial full snapshot содержит только записи 12 разрешённых SCHEMA-типов;
- `WIFI_SCAN` и `WIFI_KNOWN` отсутствуют в Web snapshot;
- `SNAPSHOT_STATE` не возвращает model-типы;
- `/model.bin`: magic `HASM`, ver 1, count, records только model-типов;
- model rebuild после изменения model-записи;
- `REMOVE` уходит при факте удаления без Web cache;
- slow client: 64 frames И 16 KB, закрытие сессии;
- `seq` независим для двух клиентов;
- send не удерживает mutex во время `httpd_ws_send_frame_async`;
- Display initial read не требует скрытого `main::rebuild`;
- Display property/automation/Wi-Fi targeted update не делает полный rebuild;
- Display decoders используют `ha_model`, не raw offsets.

## 5. Implementation order

1. **6.1 Web snapshot/schema/model-state** — SCHEMA, Domain snapshot, model.bin,
   SNAPSHOT_STATE.
2. **6.2 Web delivery** — per-client seq, 64/16KB queue, REMOVE, send outside mutex.
3. **6.3 Display lifecycle** — start(domain), initial read, command path.
4. **6.4 Display decoding/updates** — ha_model decoders, targeted updates, warnings cleanup.
5. **6.5 Integration/P4** — Web+Display+Domain, slow-client/reconnect, P4 acceptance.

Никаких новых механизмов вне V2 не добавлять. Если позже понадобится V3 full-snapshot,
оформить отдельное документированное расширение протокола.