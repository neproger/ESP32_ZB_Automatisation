# Stage 6 — Architecture Decisions: Web + Display

> Статус: **Stage 6.1–6.5 implemented; physical touch callback deferred**.
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
- Bounded delta queue: одновременно **64 кадра и 16 KB**; превышение закрывает сессию.
  Snapshot идёт отдельной очередью и не ограничен этим бюджетом (иначе полный
  `SNAPSHOT_STATE`/`SNAPSHOT` на 32 устройства закрывал бы клиента при подключении).
- `MAX_CLIENTS = 8` как V2. `max_open_sockets` считает только клиентские
  сокеты; httpd добавляет 3 внутренних, т.е. 8 + 3 = 11 из 16 LWIP, и 5
  остаются под DNS/SNTP/погоду/повторные подключения браузера.
- Регистрация клиента и сборка snapshot выполняются атомарно под одним lock
  очередей: дельта не может потеряться между регистрацией и чтением Domain.
- `/model.bin` собирается из Domain на каждый запрос; кэш MODEL удалён, поэтому
  клиент не может получить blob старше момента запроса.
- `REMOVE` строится из Domain fact и broadcast независимо от какого-либо Web cache.
- Web-owned MODEL cache удаляется: snapshot, delta и REMOVE не зависят от cache.
- WebSocket send выполняется вне внутреннего mutex; очередь защищается mutex,
  `httpd_ws_send_frame_async` — нет.
- Медленный клиент закрывается; после reconnect получает свежий snapshot.
- Domain subscription Web использует V2-маску `ENTITY_UPSERTED | ENTITY_REMOVED | EVENT`.
- `EVENT` — canonical 24-byte `ha_event_t`, читается из Domain payload и отдаётся
  verbatim; не синтезируется из полей `Fact`.
- Команды проверяют точные V2-размеры args; PUT дополнительно проверяет record по SCHEMA.
- Web drain выполняется в собственной задаче `web-client` (`ha-web-service`), не в main.
- Web transport вынесен в `ha-web-service`, Display — в `ha-display`.

### Web code structure

- `ha-web-service/src/lib.rs` — HTTP/WS lifecycle and canonical projection facade.
- `ha-web-service/src/session.rs` — per-client queues, `seq`, broadcast/flush.
- `ha-web-service/src/snapshot.rs` — full/state snapshots and `/model.bin`.
- `ha-web-actions/src/lib.rs` — bridge from Web commands to Domain/config/Device.
