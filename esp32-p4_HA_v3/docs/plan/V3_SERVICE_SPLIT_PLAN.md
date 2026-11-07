# V3: довести разделение крейтов до V2

Статус: **этапы 1–10 готовы**; 9 — короткая приёмка (24h soak/release не делали).
Базовая точка: V3 HEAD + safe Wi-Fi patch (no `get_ap_info`), 40 МГц,
`STANDARD_CONFIG_VERSION=5`, web/C6 работают.

Цель — не переставить файлы, а довести владение до V2: `ha-app` только
собирает; каждый сервис владеет своей задачей, inbox и командами; состояние
adapter-слоя приватно.

## Целевая картина

```text
main.rs        IDF init, store init, Domain + таблицы, restore,
               dispatcher, запуск сервисов, sleep. Больше ничего.

ha-display     свой channel + subscribe + display-client (nav/drain/tick)
ha-web-service свой channel + subscribe + web-client (drain/flush)
ha-wifi        свой task + регистрирует COMMAND_WIFI_SCAN/CONNECT
ha-zigbee      свой task + capability drain + регистрирует
               COMMAND_PROPERTY / COMMAND_DISCOVERY
ha-config      seed_defaults + put/remove с origin
ha-device      register/bind/rename/remove + rename_entity
ha-automation  уже сам
ha-system      уже сам

runtime.rs     удалить
intake.rs      удалить (web Actions зовут Domain/Device/config напрямую)
web_actions.rs тонкий Actions-адаптер, без C-вызовов
```

V2-эталон: `main/app_main.c` делает только descriptors + dispatcher +
`*_start`; `web_start`, `wifi_start`, `zigbee_start` сами подписываются и
регистрируют команды. V3 сейчас отличается только `runtime::clients`,
`runtime::prepare`, `runtime::handle_intake` и прямым
`web → platform::zigbee_open_network`.

## Этапы

- [ ] **0. Базовая точка.** Зафиксировать текущее стабильное состояние
      (коммит/тег), сохранить safe-патч Wi-Fi, записать baseline: `/`,
      `/model.bin`, `tx_bytes`, C6 reports. Wi-Fi/SPI не трогать.
- [x] **1. Display владеет задачей.** `ha-display::start(domain)` сам создаёт
      channel/subscribe и ставит LVGL-таймер (`lv_timer_create`, 20 мс),
      который в задаче LVGL делает nav/drain/tick — как V2
      (`drain_timer_cb`), без отдельного Rust-потока. Убрать display из
      `runtime::clients` и `display_rx` из main.
      Файлы: `ha-display/src/lib.rs`, `ha-app/main.rs`, `runtime.rs`.
      **Результат:** отдельный `display-client` поток валил web после первого
      крупного HTTP-ответа (система зависала, ping пропадал). Переход на
      LVGL-таймер + правильный порядок Wi-Fi → Web дал стабильные
      `/` 200, `/app.js` 200, `/model.bin` 200, ping 4–7 мс (3 круга + 30 с).
- [x] **1a. Порядок запуска Wi-Fi → Web (как V2).** `ha_wifi::start` сначала
      грузит known AP и синхронно инициализирует C Wi-Fi стек, затем main
      запускает web. Устраняет web-до-Wi-Fi и асинхронную инициализацию netif.
      Файлы: `ha-wifi/src/lib.rs`, `ha-app/src/main.rs`.
- [x] **2. Web владеет задачей.** `ha-web-service::start(domain, actions)` сам
      создаёт channel/subscribe и спавнит `web-client` (drain/flush).
      `drain/flush` сделаны приватными. Web убран из main/runtime.
      **Результат:** отдельный web-поток сам по себе валил HTTP, пока рядом
      жил `runtime::clients` (20 мс poll capabilities/subscriber_drops).
      После выноса capabilities в Zigbee (3a) web-поток стабилен:
      `/` 242 B, `/app.js` 24886 B, `/model.bin` 3114 B — 200, 17–124 мс;
      ping 4–5 мс, 3 круга подряд.
- [x] **3a. Zigbee владеет capabilities.** `ha-zigbee-service::run` сам
      вычитывает `take_capabilities` и применяет их к Domain. `runtime::clients`
      удалён, main больше не держит capability-поток. Это убрало конфликт с
      web-потоком.
- [x] **3b. Zigbee владеет командами.** `COMMAND_PROPERTY` регистрируется
      внутри `ha-zigbee-service::start`; `State` приватный; `start` возвращает
      только `Handle`, main больше не держит `Arc<Mutex<State>>`.
      `Handle::submit/open_network` и `COMMAND_DISCOVERY` — этап 7.
      Проверка: C6 configure `exec op=1..4 Confirmed`, `version=5`; web стабилен.
- [x] **4. Wi-Fi сам регистрирует команды.** `ha-wifi::start` регистрирует
      `COMMAND_WIFI_SCAN`/`CONNECT`; main больше не знает про Wi-Fi команды.
      Проверка: scan/connect с дисплея и web (проверено косвенно: web/ping
      стабильны, регистрация без ошибок).
- [x] **5. Config/Device владеют записями.** `ha-config::seed_defaults`,
      `put_record(origin)`, `ha-device::rename_entity`; `runtime::prepare`
      удалён; main вызывает `ha_config::seed_defaults`.
      Проверка: host-тесты `ha-config`/`ha-device` (19 passed), web стабилен.
- [x] **6. Web Actions без intake.** `web_actions` вызывает Domain/Device/config
      напрямую; `intake.rs` и `runtime.rs` удалены; команды регистрируются
      сервисами (Zigbee/Wi-Fi). `ha-config::remove` владеет каскадами
      group/automation.
      Проверка: web `/`+`/app.js`+`/model.bin` — 200, ping 5–6 мс.
      Пользовательскую команду/rename нужно проверить в UI.
- [x] **7. ABI-команды как в V2.** Добавлен `COMMAND_DISCOVERY=3`; web постит
      его через Domain, C `zb_poc_open_network` вызывает только Zigbee-сервис.
      Entity rename остаётся прямым вызовом `ha-device`. `platform` больше не
      содержит Zigbee C-вызовов. Проверка: сборка/прошивка, web стабилен;
      discovery в UI ещё не нажимали.
- [x] **8. Lean/cleanup.** `ha-web` test-only helpers под `#[cfg(test)]`;
      `docs/architecture/V3_LAYERS.md` обновлён под фактическое владение.
      `DomainHandle::with_mut` оставлен: он используется тестами.
      Убраны неиспользуемые direct deps: `ha-app` — `ha-model`, `ha-web`,
      `zigbee-devices`, `z2m-rs`, `zigbee-exec`, `zigbee-frames`;
      `ha-display`/`ha-zigbee-service` — `ha-model`. Оставлены `embassy-time`
      (link feature `esp-idf-svc`) и `esp-idf-svc` в display (build order
      `esp-idf-sys`).
      Проверка: `cargo test --workspace` — host-тесты passed; build/flash ok.
- [x] **9. Короткая приёмка.** `cargo test --workspace`, debug build, прошивка,
      60 s soak (6 кругов): `/`+`/app.js`+`/model.bin` — 200 каждый круг,
      ping true. 24-часовой soak, release и ручной discovery/toggle — отдельно.

После этапов 1–9, отдельно:

- [x] **10. Граница Wi-Fi.** C `wifi_task` — единственный владелец radio,
      autoconnect, reconnect/backoff и state (0 idle, 1 scanning, 2 connecting,
      3 connected, 4 failed). Rust `ha-wifi` больше не держит `next_status_state`
      и таймаут connect; только ставит scan/connect в очередь и публикует
      Domain status/scan. Добавлен `ha_wifi_state()` FFI.
      Проверка: сборка/прошивка, web 200, GROUP_PUT, ping 10 мс.
- [x] **11. Второй заход: убрать последние межсервисные ручки.**
      `web_actions` больше не держит глобальный `Context` и Zigbee `Handle`:
      `WebActions` владеет только `DomainHandle`, создаётся app-lifetime
      `Box::leak`. Device removal перенесён в `ha-device::remove` (prepare →
      remover → canonical cascade); Zigbee-сервис больше не имеет `Handle`,
      а `REMOVALS` из remover'а вычитывает его run-loop и ставит leave.
      Проверка: `ha-device` 15 tests passed, firmware build/flash, web 200,
      GROUP_PUT, ping 13 мс.
- [x] **12. Третий заход: разложить остатки `ha-app`.**
      `web_actions` → отдельный адаптер `ha-web-actions`; `domain_tables` →
      `ha-schema` (workspace crate); `platform`/`p4_probe` → `ha-platform`;
      C `zb_poc_start`/`zb_poc_run` перенесены в `ha-zigbee-service::start`.
      В `ha-app` остался только `main.rs` — bootstrap без модулей.
      Проверка: `ha-schema` 3 tests, build/flash, web 200, ping 13 мс.

## Правила

- Один этап = один маленький коммит; перед началом — тег текущего стабильного
  состояния.
- После каждого этапа: `cargo test --workspace`, clippy, debug build, flash,
  60 s smoke (web, display, C6, Wi-Fi).
- Wi-Fi и SPI не трогаем: 40 МГц, no `get_ap_info`,
  `STANDARD_CONFIG_VERSION=5`.
- Никаких новых traits/слоёв: используем существующие `DomainHandle`,
  `Handle`, `Actions`.
- Если этап дважды ломает железо — откат этапа и разбор, не идём дальше.
- `runtime.rs` и `intake.rs` должны исчезнуть к этапу 6; `main.rs` — только
  bootstrap.
- Полный сохранённый патч `ha-changes.patch` используем как источник хунков
  для этапов 1–5, но применяем по одному, не целиком.

## Журнал

- 2026-11-07: план создан; этап 1 (display) сначала сделан отдельным
  `display-client` потоком. Web умирал на первом крупном HTTP-ответе
  (`httpd_sock_err: error in send : 11`), затем ping пропадал; ранний и
  поздний старт потока не помогали.
- 2026-11-07: найдено отличие от V2 — V2 запускает Wi-Fi до Web, V3 запускал
  Web до Wi-Fi; C Wi-Fi стек инициализировался асинхронно уже после web.
  Исправлено: `ha_wifi::start` синхронно поднимает C стек, main стартует
  Wi-Fi раньше web.
- 2026-11-07: display drain/tick переведён в LVGL-таймер (задача LVGL), как
  `drain_timer_cb` в V2. Отдельного display-потока больше нет. Проверено:
  `/` 242 B, `/app.js` 24886 B, `/model.bin` 3114 B — 200, время 24–107 мс;
  ping 4–7 мс; повторные запросы через 30 с стабильны.
- 2026-11-07: этап 2 (web-поток) сначала снова ронял HTTP. Причина — не сам
  web-поток, а параллельный `runtime::clients`, который каждые 20 мс дёргал
  capabilities и `subscriber_drops`. После переноса capability drain в
  `ha-zigbee-service::run` и удаления `runtime::clients` web-поток в
  `ha-web-service::start` стабилен; 3 круга `/`+`/app.js`+`/model.bin` — 200,
  ping 4–5 мс.
- 2026-11-07: этапы 3b и 4. `COMMAND_PROPERTY` и Wi-Fi scan/connect
  регистрируются сервисами; main больше не регистрирует команды и не держит
  Zigbee `State`. Сборка/прошивка: configure `exec op=1..4 Confirmed`,
  `version=5`; после прогрева 3 круга web — 200, ping 6 мс. Первый заход
  после перезагрузки был медленным (app 9 с), затем стабильно; нужен soak.
- 2026-11-07: этапы 5 и 6. Seed defaults и entity rename переехали в
  `ha-config`/`ha-device`; `put_record` получил origin. Web Actions звонят
  Domain/config/Device напрямую; `intake.rs` и `runtime.rs` удалены. Host-
  тесты 19 passed; web 2 круга — 200; ping 5–6 мс; образ 4,136,672 B.
  Пользовательский toggle/rename/CRUD ещё не прогнан вручную.
- 2026-11-07: этап 7. `COMMAND_DISCOVERY=3` добавлен; Zigbee-сервис сам
  регистрирует обработчик и вызывает C `zb_poc_open_network`; web Actions
  постит команду через Domain. `platform` больше не содержит Zigbee
  network-вызова. Web после прошивки: 2 круга — 200, ping 5 мс.
- 2026-11-07: этапы 8–9. Обновлён `V3_LAYERS.md`; test-only builder'ы
  `ha-web` под `#[cfg(test)]`; `with_mut` оставлен для тестов.
  `cargo test --workspace` — все passed; debug build/flash; 60 s soak —
  6/6 кругов `/`+`/app.js`+`/model.bin` 200, ping true.
- Итог: `ha-app` — только bootstrap; `runtime.rs`/`intake.rs` удалены;
  display — LVGL timer, web — свой task, Zigbee — команды/capabilities,
  Wi-Fi — свои команды, config/device — свои записи.
- 2026-11-07: переименование группы (GROUP_PUT) вызывало
  `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
  (esp_task_stack_is_sane_cache_disabled)` и ребут: web Actions писали flash
  из httpd-задачи, чей стек был в PSRAM/8 КБ. Исправлено: httpd stack 16 КБ,
  `task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. Проверено реальным
  GROUP_PUT: title обновился, ребута/assert нет, ping 6–8 мс.
- 2026-11-07: этап 10. Wi-Fi state/reconnect/backoff полностью в C
  `wifi_task`; Rust удалил `next_status_state`/`last_connect`. `ha_wifi_state()`
  отдаёт V2 state в Domain. Прошивка: web 200, GROUP_PUT, ping 10 мс.
- 2026-11-07: дисплей не получал время/погоду. Причины: (1) `sntp.rs`
  передавал временный `CString` в `esp_sntp_setservername`, а IDF хранит
  указатель — заменено на static `b"pool.ntp.org\0"`; (2) `geo::wait_clock`
  ждал вечно, поэтому без SNTP не публиковались location/weather и
  `run_clock` не стартовал. Добавлен 10s timeout: weather публикуется даже
  при позднем SNTP; время публикуется, когда sync завершится.
  Лог: `clock not ready after SNTP; continuing` → `weather cond=5` →
  `local clock ready`.
- 2026-11-07: время на дисплее всё ещё не появлялось после публикации facts.
  Display теперь раз в 500 мс сам читает `SYSTEM_HOUR/MINUTE` из Domain и
  обновляет статус-бар (как V2 `ui_status_bar_apply`) — не зависит от
  потерянного/позднего fact. Проверено логом `display: clock 23:15` и
  визуально: время и погода на экране.
- 2026-11-07: второй архитектурный заход. Убраны `web_actions` global
  `Context`/Zigbee `Handle`; `ha-zigbee-service` больше не отдаёт `Handle`.
  Device removal теперь `ha_device::remove`; Zigbee run-loop вычитывает
  `REMOVALS` и ставит leave. Web Actions владеет только Domain. Проверка:
  build/flash, `/`+`/app.js`+`/model.bin` 200, GROUP_PUT, ping 13 мс.
- 2026-11-07: чистка direct deps. Убраны 6 лишних из `ha-app` (`ha-model`,
  `ha-web`, `zigbee-devices`, `z2m-rs`, `zigbee-exec`, `zigbee-frames`) и
  `ha-model` из `ha-display`/`ha-zigbee-service`. `embassy-time` и
  `esp-idf-svc` в display оставлены: нужны линковке/build-order.
  Полная сборка прошла; прошивка: `/` 200, `/app.js` 200, `/model.bin` 200,
  ping 9 мс.
- 2026-11-07: третий заход. `web_actions` вынесен в `ha-web-actions`;
  `domain_tables` — в `ha-schema`; `platform`/`p4_probe` — в `ha-platform`;
  Zigbee C start/run — в `ha-zigbee-service::start`. В `ha-app/src` остался
  только `main.rs`. `ha-schema` 3 tests passed; build/flash; web 200, ping 13 мс.
- 2026-11-07: SNZB-01P клики. Причина: сгенерированный профиль
  `sonoff.snzb01p` содержал только battery, без `ewelinkAction` (genOnOff
  commandToggle/On/Off). Добавлены `Converter::CommandId`, expose
  single/double/hold, genOnOff bind и version=2 конфига. Boot:
  `restored profile=sonoff.snzb01p`, `configure: step=0 op=1` (bind
  Expired — sleepy device, retry on wake). Тест
  `snzb_01p_ewelink_commands_decode_actions` passed.

## V2 → V3

| V2 | V3 |
|---|---|
| `main/app_main.c` | `ha-app/main.rs` + `domain_tables.rs` |
| `domain` | `ha-domain` |
| `ha_model` | `ha-model` |
| `mstore` | `micro-db` |
| `device` | `ha-device` |
| `automation` | `ha-automation` |
| `web` | `ha-web` + `ha-web-service` |
| `system` | `ha-system` |
| `wifi` | `ha-wifi` + `native_net` |
| `display` + `display_p4` | `ha-display` + `native_display` |
| `zigbee` | `ha-zigbee-service` + `zigbee-*` |
| `sys` | общие error-типы |

## Definition of done

`ha-app/main.rs` не содержит каналов, drain, бизнес-логики и C-вызовов; каждый
сервис владеет задачей/inbox/командами; web не зовёт Zigbee C напрямую;
Zigbee `State` приватный; host-тесты и железо зелёные.
