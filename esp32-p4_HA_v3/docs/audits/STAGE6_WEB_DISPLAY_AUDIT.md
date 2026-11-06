# Stage 6.0 — Web + Display Audit

> Статус: **audit only; implementation не начат**.
> Источники V2: `esp32-p4_HA_v2/docs/services/WEB.md`,
> `WEB_PROTOCOL.md`, `esp32-p4_HA_v2/web/src/*`, `esp32-p4_HA_v2/display/src/*`.
> Rust: `crates/ha-web`, `crates/ha-app/src/web.rs`, `crates/ha-app/src/display.rs`,
> `crates/ha-app/src/main.rs`.

## 1. Граница Stage 6

Web и Display остаются клиентами canonical Domain и не владеют source of truth.
Display не получает прямых Wi-Fi C API; Wi-Fi данные читаются из Domain.
Canonical ABI, Zigbee/Device/Automation/System не переоткрываются.

## 2. Web — соответствие V2

| Область | V2 | Rust сейчас | Gap |
|---|---|---|---|
| Snapshot | `web_projection_snapshot()` итерирует Domain по явному `SCHEMA` | `snapshot_frames()` собирает snapshot из `MODEL` cache | **GAP:** snapshot зависит от Web-кэша, а не от Domain |
| Schema | Явный список типов и размеров; `WIFI_SCAN` не отдаётся Web | `is_web_visible()` исключает только `WIFI_KNOWN`; всё остальное с `u8`-типом идёт в модель | **GAP:** нет явного Web schema; возможны лишние типы |
| Model/state | `/model.bin` — static blob (HA SM) из Domain; `SNAPSHOT_STATE` отдаёт только non-model | `/model.bin` всегда 404; `SNAPSHOT_STATE` отдаёт полный Web snapshot | **GAP:** нет V2 model/state split и model.bin |
| Delta upsert | На `ENTITY_UPSERTED` читает Domain и broadcast canonical key/record | Читает Domain, обновляет cache, broadcast только при изменении | В целом aligned; cache нужен только для dedupe, не для snapshot |
| Delta remove | Broadcast `REMOVE(type,key)` независимо от локального кэша | Удаляет из cache; если записи в cache нет — `REMOVE` не уходит | **GAP:** потеря remove после eviction/позднего подключения |
| Slow client | Per-client queue: 64 frames + 16 KB; при переполнении закрыть | Per-client `VecDeque` 64 frames без byte cap; при 64 закрыть | **GAP:** нет byte cap; лимит клиентов 4 vs V2 8 |
| Sequence | Per-client `next_seq` | Один глобальный `SEQ` на все WebSocket-сессии | **GAP:** seq не per-client |
| Sending | `web_sessions_step()` отправляет вне lock; `websocket async` | `flush()` держит `CLIENTS` mutex во время send | **GAP:** возможная блокировка/контеншн; V2 выносит send из lock |
| Model cache | Нет отдельного Web-owned model; snapshot из Domain | `MODEL` до 768 записей с eviction; snapshot из него | **GAP:** Web владеет состоянием, snapshot может быть неполным |
| Commands | `web_commands` переводит в Domain/Device | Команды уже идут в intake → main → Domain; CRUD аналогично | Архитектурно близко; путь через composition intake, не прямой service API |
| EVENT | `ha_event_t` verbatim | EVENT 24 bytes (id/source) | Aligned по wire-смыслу; проверить точный layout в integration |

**Вывод Web:** бинарный протокол, delta и bounded queue уже есть, но snapshot/model/state
семантика и per-client delivery заметно расходятся с V2. Главный риск — `MODEL` cache
становится вторичным source of truth для начальной синхронизации.

## 3. Display — соответствие V2

| Область | V2 | Rust сейчас | Gap |
|---|---|---|---|
| Start/lifecycle | `display_start(domain)` подписывается до `build_ui`, затем создаёт LVGL timer | `display::start()` без Domain; main подписывает Domain, вызывает `start` и затем `rebuild` | **GAP:** ownership/lifecycle split; initial read зависит от main |
| Initial build | Строит UI и сразу применяет Domain state/capabilities | `rebuild(domain)` строит страницы после `start` | Функционально близко; оформить как Display-owned initial read |
| Domain facts | Подписка на upsert/remove/event; event игнорируется | Display inbox получает `Filter::all`; drain обрабатывает upsert/remove | Aligned; event корректно не рендерится |
| Group/entity/caps | Dirty → пересобрать текущий экран | `rebuild_needed` для group/entity/caps/group item/logical device | Aligned по смыслу |
| Automation/group automation | `AUTOMATION` обновляет switch на месте; `GROUP_AUTOMATION` обновляет плитку без полной перестройки | Оба типа ставят `rebuild_needed` | **GAP:** лишняя полная перестройка страницы |
| Property state | `ui_page_apply_key()` обновляет конкретный widget | `on_state()` обновляет widget по `(entity, property)` | Aligned; проверить color group |
| Wi-Fi screen | `ui_wifi_apply()` обновляет status/list из Domain | `refresh_wifi()` пересобирает всю Wi-Fi страницу | **GAP:** нет targeted update; лишние LVGL операции |
| Wi-Fi API | Только Domain | Только Domain | Aligned |
| Commands | Callback → `domain_post` / Domain entity_put | Callback → `intake::*` → main loop → Domain/automation | **GAP:** command path не прямой service API; латентность/ответственность composition |
| Canonical decode | `ha_model` records/helpers | Капы/имена/состояния частично через raw offsets (`bytes[0]`, `4+24*i`, `8..40`) | **GAP:** ручной разбор вместо `ha_model` структур |
| Settings/status bar | Domain settings/weather/location | Domain `SettingsRecord`/`WeatherRecord`/`LocationRecord` | Aligned |
| Threading | LVGL task + Domain subscription timer | Отдельный display-drain thread + lock; LVGL tick в main | Функционально допустимо; оформить ownership/task contract |
| Warnings | — | `#![allow(warnings, clippy::all)]` | Качество/скрытые ошибки; не менять вслепую |

**Вывод Display:** основная Domain-модель уже есть; это не переписывание с нуля.
Основные gaps — lifecycle ownership, прямой command path, `ha_model` decoding и
targeted updates для Wi-Fi/automation.

## 4. Что уже подтверждено как aligned

- Web: canonical binary framing, WebSocket, canonical records без DTO, команды/CRUD,
  delta по Domain facts, EVENT, скрытие `WIFI_KNOWN`.
- Display: группы/items/capabilities/state/weather/location/settings из Domain,
  capabilities-driven widgets, навигация, отсутствие прямых Wi-Fi C calls.
- Оба используют bounded inbox/queue; медленный клиент не должен блокировать Domain.

## 5. Утверждённые решения для Stage 6 spec

1. Web snapshot: Domain по явному SCHEMA; Web-owned `MODEL` cache удаляется.
2. `/model.bin` и `SNAPSHOT_STATE` восстанавливаются в V2-виде; full-snapshot fallback
   для state не сохраняется.
3. Web queue: 64 frames + 16 KB, per-client `seq`, send вне mutex.
4. Display: initial read и command submission в Display service API; `main` только wiring.
5. Display: `ha_model` decoders; targeted update для Wi-Fi/automation; rebuild только
   для состава/имени/capabilities; убрать глобальный `allow(warnings)`.
6. Порядок: Web snapshot/session, Web delivery, Display lifecycle, Display decoders,
   integration/P4; canonical ABI не меняется.

Полный текст решений: [STAGE6_ARCHITECTURE_DECISIONS.md](../plan/STAGE6_ARCHITECTURE_DECISIONS.md).

## 6. Предлагаемый порядок работ

1. **6.1 Web snapshot/schema** — Domain snapshot, model/state decision, remove semantics.
2. **6.2 Web delivery** — per-client seq/byte cap, send outside lock, slow-client test.
3. **6.3 Display ownership/initial read** — Domain-based start, command path.
4. **6.4 Display decoders/targeted updates** — ha_model, Wi-Fi/automation incremental.
5. **6.5 Integration/P4** — Web+Display+Domain, snapshot/delta/slow client, reboot.

Решения §5 утверждены в [STAGE6_ARCHITECTURE_DECISIONS.md](../plan/STAGE6_ARCHITECTURE_DECISIONS.md).
Следующий шаг — 6.1 Web snapshot/schema/model-state.