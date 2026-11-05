# Stage 5 — Scope and Dependencies: Wi-Fi + System

> Статус: **implementation complete; clean build, initial P4 smoke, WEATHER_CHANGED и autoconnect passed; reconnect/backoff pending**.
> Основание: [V2_TO_RUST_PORTING_PLAN.md](../plan/V2_TO_RUST_PORTING_PLAN.md),
> V2 `docs/services/WIFI.md`, `docs/services/SYSTEM.md`,
> `docs/architecture/V3_LAYERS.md`.
> Код не менялся.

## 1. Границы Stage 5

| В scope | Не в scope |
|---|---|
| Wi-Fi service behavior: scan/connect/known/status, autoconnect, reconnect/backoff | Web/Display migration, UI/navigation |
| Wi-Fi commands and canonical WIFI_* records via Domain | Zigbee/Device/Automation |
| System: SNTP, time properties, tick events, GeoIP, weather | Canonical ABI/Domain persistence changes |
| Correct `SOURCE_SYSTEM`/`SOURCE_WIFI` metadata | New services/traits/mechanisms not in V2 |
| Remove System/Wi-Fi business publication from `main` | C3 firmware changes unless a hardware blocker requires them |
| Host tests + P4 smoke | Stage 3 hardware acceptance |
| Перенос Wi-Fi/System service ownership в `ha-wifi`/`ha-system` | Ownership остаётся в `ha-app` до Stage 7 |

Display не мигрируем: текущий Display уже читает `WIFI_SCAN`/`WIFI_STATUS`
из Domain и не должен получить новых прямых radio calls.

## 2. Зависимости

- **Domain + `ha-model`**: canonical WIFI_* / SYSTEM / LOCATION / WEATHER,
  facts, commands, source ids.
- **native_net / ESP-Hosted / C3**: единственный radio path для Wi-Fi.
- **P4 + C3**: hardware gate Stage 5; C3 Wi-Fi CP не связан с Zigbee C6.
- **System → network/IP**: System ждёт IP через согласованный Domain-контракт
  (`WIFI_STATUS`/событие), сам владеет SNTP и не вызывает Wi-Fi API напрямую.
- **Stage 4**: завершён; System tick/state events уже потребляются Automation,
  но Stage 5 не меняет Automation.
- **Stage 3 hardware acceptance**: отдельный gate, не блокирует Stage 5.
- **Stage 1 hardware persistence**: остаётся частичным gate; Stage 5
  проверяет `wifi_known` и location persistence на P4 отдельно.

## 3. Текущее состояние и gaps

### Wi-Fi

Сейчас: `ha-app/wifi.rs` + `native_net/ha_wifi.c`; service task владеет радио,
Domain содержит `WIFI_STATUS`/`WIFI_SCAN`/`WIFI_KNOWN`, Display читает Domain.

Gaps:

- `WIFI_STATUS.state` публикуется только как `connected=3` / idle=0; V2
  различает scanning/connecting/error.
- Scan record теряет `auth` (и channel не входит в canonical record);
  проверить, что V2-поля заполняются в пределах canonical формы.
- known id allocation: current first-free id vs V2 `max_id+1`.
- SNTP сейчас запускается из Wi-Fi path (`ha_time_start`); по V2/V3
  SNTP — ответственность System.
- `COMMANDS` — глобальный static; lifecycle/ownership сервиса нужно
  зафиксировать (не менять incidental).
- Autoconnect/backoff/watchdog живут в `native_net/ha_wifi.c`; поведение
  V2 сохранить, но проверить на P4/C3 (host-test только selection logic).
- `clear_scan` после успешного connect: проверить соответствие V2.

### System

Сейчас: `ha-app/system.rs` делает GeoIP/weather fetch и кладёт `Update` в
очередь; `main` публикует location/weather, time properties и MINUTE/WEATHER
events.

Gaps:

- time states и tick events публикуются из `main`, не из System task.
- `publish_event` в `main` помечает system events как `SOURCE_ZIGBEE`.
- отсутствуют HALF_HOUR/HOUR/DAY ticks.
- `system::take` polling в `main`; System должен сам владеть Domain publication.
- location пишется через `config::put_record` с `SOURCE_ZIGBEE`; нужен
  `SOURCE_SYSTEM` (или source-aware helper).
- SNTP ownership нужно перенести из Wi-Fi в System.
- weather source уже `SOURCE_SYSTEM`; сохранить.

## 4. Contract decisions for Stage 5

1. **SNTP owner:** System, после появления IP; Wi-Fi не запускает SNTP.
2. **Wi-Fi owner:** service task + radio; System не вызывает Wi-Fi API.
3. **Domain only:** consumers (Display/Web/Automation) не читают C radio
   напрямую; Wi-Fi публикует canonical records.
4. **Source metadata:** System time/location/weather/ticks — `SOURCE_SYSTEM`;
   Wi-Fi records — `SOURCE_WIFI`.
5. **V2 ticks:** MINUTE/HALF_HOUR/HOUR/DAY и WEATHER_CHANGED из System task.
6. **Known AP:** flash через Domain; radio RAM list синхронизируется из Domain.
7. **Ownership move:** Stage 5 переносит service ownership в `ha-wifi` и
   `ha-system`. `ha-app` остаётся composition root: создаёт зависимости,
   конфигурирует и запускает сервисы, но не публикует WIFI_*/system facts.
   Детали: [STAGE5_OWNERSHIP_DECISION.md](../plan/STAGE5_OWNERSHIP_DECISION.md).

## 5. Предлагаемый порядок

1. Зафиксировать ownership/contracts (approved).
2. `ha-wifi`: known/status, затем scan/connect, затем autoconnect/reconnect/backoff.
3. `ha-system`: SNTP + time properties, ticks/events, GeoIP/location/weather.
4. Удалить дублирующую System/Wi-Fi публикацию из `main`.
5. Host tests: contracts, events, reconnect, time/source.
6. P4/C3 smoke: scan/connect/known restart, reconnect/backoff, SNTP/GeoIP/weather,
   tick/event source, Display reads Domain.

## 6. Hardware checks

- Wi-Fi: scan; connect; known restore after reboot; strongest known autoconnect;
  disconnect/reconnect; backoff; status states; no direct C call from Display.
- System: SNTP after IP; time properties; MINUTE/HALF_HOUR/HOUR/DAY ticks;
  GeoIP/location; weather refresh; `SOURCE_SYSTEM`; Automation sees ticks.
- Persistence: `wifi_known` and location survive reboot.
- C3 Wi-Fi CP unaffected by Zigbee; Zigbee Stage 3 not reopened.

## 7. Out of scope

- Web/Display migration, UI/widgets, snapshot/delta.
- Zigbee/Device/Automation changes.
- Canonical ABI, Domain storage/partition changes.
- New abstractions or features absent from V2.