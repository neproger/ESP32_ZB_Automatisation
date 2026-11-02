# Аудит Wi-Fi Layer: V2 → Rust

> Статус: **аудит перед реализацией**. Код не менялся.
> V2: `esp32-p4_HA_v2/wifi/`, `docs/services/WIFI.md`.
> Rust: `crates/ha-app/src/wifi.rs`, `native_net/ha_wifi.c`,
> `domain_tables.rs`, `display.rs`, `system.rs`, `web.rs`.

## 1. Контракт V2

### 1.1. Публичный вход

- `wifi_start(domain)` — единственный экспорт.
- Команды: `HA_CMD_WIFI_SCAN=4`, `HA_CMD_WIFI_CONNECT=5`.
- Entity: `WIFI_SCAN=10`, `WIFI_KNOWN=11`, `WIFI_STATUS=12`.
- Synthetic key: `HA_WIFI_DEVICE_ID = 0x5749464900000001`.
- Source: `DOMAIN_SOURCE_WIFI=5`; error layer `SYS_LAYER_WIFI=7`.

### 1.2. Данные и ёмкости

| Данные | Ёмкость/размер | Backing |
|---|---|---|
| `WIFI_SCAN` | 16 AP, 580-byte record | RAM |
| `WIFI_KNOWN` | 8, 96-byte record | RAM+FLASH, key `wifi_known` |
| `WIFI_STATUS` | 1, 36-byte record | RAM |
| `wifi_connect_args_t` | 96 bytes: SSID[32], password[64] | command args |
| Task | 8192 stack, priority 4, queue 4 | — |

### 1.3. Поведение

- Init: `wifi_start` registers commands and starts a task; stack init and
  autoconnect run inside the task. Start returns before radio is ready.
- Scan: active scan, 120–300 ms; blocking in C; result stored in
  `WIFI_SCAN`; auth is mapped from ESP-IDF; old scan is retained on failure.
- Connect: `ensure_known` saves SSID/password before connect; status
  `CONNECTING`; `esp_wifi_set_config` + `esp_wifi_connect`; no explicit
  disconnect command; a new connect may disconnect implicitly.
- Known: match by SSID, overwrite password; new key = `max_id + 1`; full list
  still attempts connect but does not save.
- Status: `IDLE`, `SCANNING`, `CONNECTING`, `CONNECTED`, `ERROR`;
  `connected=1` means GOT_IP, not just association.
- GOT_IP: status CONNECTED, clear `WIFI_SCAN`, reset backoff.
- Events/errors: disconnect reason mapped for logs; status ERROR on
  disconnect; no error-code field in the canonical status record.
- Reconnect: autoconnect strongest known AP; backoff 1/2/4/8/16 s; 5 s
  watchdog with two association misses and 15 s connect grace.
- Persistence: Domain owns `WIFI_KNOWN`; C keeps only a private radio copy.
- Selection: strongest known AP by SSID/RSSI; host-tested
  `wifi_select_known`.

### 1.4. Фактические ограничения V2

- Нет `wifi_stop`/deinit.
- Нет команды удаления/forget known network.
- Нет explicit disconnect command.
- Open/WPA-PSK фактически не подключаются: threshold `WPA2_PSK`.
- `connected` в scan path может означать association, а не IP.
- Пароли хранятся plaintext; Web их не отдаёт, Display known не читает.

## 2. Текущее состояние Rust

### 2.1. Init / deinit

- `wifi::start` создаёт поток, загружает Domain known → C, вызывает
  `ha_wifi_start`, затем `import_known` (C → Domain) и polling loop.
- `ha_wifi_start` идемпотентен; отдельного stop/deinit нет ни в Rust, ни в C.
- Ошибка `ha_wifi_start` завершает поток, но `COMMANDS` остаётся с мёртвым
  receiver; последующие запросы получают `NoSpace`.
- Двойной `wifi::start` не защищён.

### 2.2. Scan

- `COMMAND_WIFI_SCAN` зарегистрирован, но production его не постит.
- Display вызывает `wifi::request_scan()` напрямую; Domain command path
  мёртв.
- C scan неблокирующий; Rust polling 500 ms, публикует `WIFI_SCAN` и
  дублирует результат в `SCAN`/`take_scan` для Display.
- Auth не переносится: C читает `authmode`, но getter отдаёт только
  SSID/RSSI; `WifiScanAp.auth` остаётся 0.
- Scan не очищается на GOT_IP.
- Display читает локальный scan cache, а не Domain.

### 2.3. Connect / disconnect

- Connect path: Display → intake → `COMMAND_WIFI_CONNECT` → executor →
  `wifi::connect_from_args` → channel → C `ha_wifi_connect_ssid`.
- ABI 96 bytes совпадает с V2.
- Save known происходит после принятия запроса, до фактического
  подключения; как V2.
- Нет explicit disconnect/cancel/stop.
- `send` любой ошибки канала отдаёт `NoSpace`, а не `Busy`/
  `InvalidState`.
- C делает `esp_wifi_disconnect` перед новым connect; это implementation
  detail, не публичный endpoint.

### 2.4. Known networks и persistence

- Domain `WIFI_KNOWN` RAM+FLASH; C держит второй RAM-список.
- Boot: Domain → C, затем C → Domain import; import фактически no-op, так
  как C не грузит NVS.
- После boot C не ресинхронизируется при изменении Domain known.
- Rust выбирает **наименьший свободный key 0..7**, V2 — `max_id+1`
  начиная с 1.
- Нет remove/forget radio API; generic Domain remove может удалить
  `WIFI_KNOWN`, но C продолжит использовать старую запись.
- `ha_wifi_known_add` возвращает `-1` для duplicate; Rust игнорирует.
- **P0 security:** Web подписан на все факты и зеркалит все
  `EntityUpserted`, включая `WIFI_KNOWN` с plaintext password, в
  `MODEL` и WebSocket snapshot. Это утечка credentials любому web-клиенту.

### 2.5. Status / events / errors

- Rust публикует status только раз в 1 s из polling.
- state жёстко `3` (connected) или `0` (idle); нет `SCANNING`,
  `CONNECTING`, `ERROR`.
- `connected` = C association (`esp_wifi_sta_get_ap_info`), а не GOT_IP.
- Нет Wi-Fi-specific Domain event/error code; C только логирует.
- Display читает C status напрямую, main читает RSSI напрямую; Domain
  `WIFI_STATUS` не является единственным путём.
- SNTP запускается в C Wi-Fi bridge (`ha_wifi_start`/GOT_IP path), хотя V2
  владелец — System service.

### 2.6. Reconnect / backoff / watchdog

- C реализует autoconnect, strongest known, watchdog 5 s/2 misses/15 s,
  exponential backoff.
- V3 backoff clamp = 5 → 1,2,4,8,16,32 s; V2 clamp = 4 → max 16 s.
  Комментарий C говорит «1..16 s».
- V2 `s_reconnect_enabled` switch отсутствует; выключить reconnect нельзя.
- Если known list был пуст на boot, watchdog не пересканирует после
  появления сети в Domain.

### 2.7. Commands / ownership

- `COMMAND_WIFI_SCAN` зарегистрирован, но не используется.
- `COMMAND_WIFI_CONNECT` используется Display.
- Executors короткие, только enqueue; это соответствует V2.
- Web не поддерживает Wi-Fi provisioning; provisioning только Display;
  это соответствует текущей V2-документации.
- Mutation calls идут через Wi-Fi thread; status/RSSI reads идут из
  Wi-Fi thread, main и LVGL thread напрямую — граница «single owner»
  нарушена.

## 3. Матрица соответствия

| Функция V2 | Rust | Статус |
|---|---|---|
| Инициализация | `wifi::start`, `ha_wifi_start` | **Adapted** (start before radio; failure leaves dead queue) |
| Деинициализация | нет | **Equivalent** (V2 тоже нет) |
| Scan | C scan + Domain + `take_scan` | **Adapted** (no auth, no clear, duplicate path) |
| Scan command | registered, not used | **Incomplete** |
| Connect | Domain command → C | **Adapted** (works, but state path incomplete) |
| Disconnect | implicit in C only | **Missing** (V2 also has no public command) |
| Known networks | Domain + C RAM | **Adapted** (duplicate ownership, key differs) |
| Save credentials | Domain put on connect | **Adapted** (errors ignored; Web leak) |
| Remove credentials | generic Domain remove only | **Incomplete** (C not updated; V2 has none) |
| Connection status | 1 s polling, state 0/3 | **Incomplete** (no SCANNING/CONNECTING/ERROR; association vs IP) |
| Events/errors | logs only | **Missing** structured status/error |
| Reconnect/backoff | C | **Adapted** (32 s max vs V2 16 s; no disable) |
| Watchdog | C | **Adapted** (association, not IP) |
| Domain writes | canonical tables | **Adapted** (geometry correct; status/scan paths differ) |
| Persistence | Domain `wifi_known` | **Adapted** (C duplicate; Web leak) |
| C3/ESP-Hosted | C bridge | **Adapted** (transport correct; direct reads leak ownership) |
| Selection | C `select_known` | **Adapted** (no host test) |

## 4. Дублирующие и переходные пути

1. Scan: Domain `WIFI_SCAN` + `SCAN`/`take_scan` for Display.
2. Status: Domain `WIFI_STATUS` + direct C reads in Display/main.
3. Known: Domain records + C RAM list, synchronized только на boot/connect.
4. Generic Domain remove vs отсутствие radio remove.
5. Зарегистрированная, но недостижимая scan command.
6. C → Domain `import_known` — мёртвый transitional path.
7. SNTP в Wi-Fi bridge вместо System service.
8. C comments/doc про NVS known APs устарели: C NVS list не хранит.

## 5. Приоритеты реализации

### P0 — security / data path

- Не отдавать `WIFI_KNOWN.password` в Web `MODEL`/WS snapshot.
- Убрать duplicate scan/status paths: Display читает Domain; scan идёт
  через `COMMAND_WIFI_SCAN`.
- Ресинхронизировать C known при изменении Domain; определить поведение
  generic remove (либо запретить, либо добавить radio remove как явное
  расширение).

### P1 — V2 behavior

- Ввести состояния `IDLE/SCANNING/CONNECTING/CONNECTED/ERROR`.
- `connected` считать по GOT_IP, а не association.
- Перенести scan `auth`.
- Очищать `WIFI_SCAN` на GOT_IP.
- Обрабатывать ошибки connect/scan/status в Domain или явном status.
- Исправить key selection (`max_id+1`, как V2) и full-list semantics.
- Ограничить backoff 16 s, как V2; обновить комментарий.
- Решить init failure/restart: не оставлять мёртвый queue.

### P2 — технические адаптации

- Timeout/health check для hosted link.
- Host test для `select_known`, `encode/decode_connect`, known key,
  status/scan mapping.
- SNTP ownership: System service, не Wi-Fi bridge.
- Обновить `docs/build/WIFI.md` и комментарии C про NVS.

## 6. Host-testable

- `encode_connect`/`decode_connect`: 96 bytes, NUL, empty SSID, truncation.
- known key selection/update/full list.
- Domain→C→Domain known merge.
- status mapping: IDLE/CONNECTED, association vs IP.
- scan mapping: count/16 cap, empty SSID, auth.
- command channel full/disconnected.
- Web redaction of `WIFI_KNOWN`.
- `select_known` parity with V2 host test.

## 7. Hardware-only

- C3 ESP-Hosted SPI bring-up and link recovery.
- real scan/connect WPA2/WPA3/open.
- autoconnect strongest AP, reboot persistence.
- disconnect/backoff/watchdog timing.
- DHCP/IP vs association status.
- Display provisioning and live status.
- Web plaintext credential exposure after redaction.
- removal/forget and repeated init/stop.
- NVS coexistence (host/Zigbee/CP).

## 8. Итог

Wi-Fi перенесён частично. Основные риски — plaintext credentials в Web,
дублирование scan/status/known, отсутствие V2 state machine и auth/scan
clear. Реализацию начинать после принятия P0-решений; сначала нужен
аудит и фиксация scope, кода в этом шаге нет.


## 9. Wi-Fi Parity & Security scope

Статус: **шаги 1 (security), 2 (single scan/status path) и 3
(known consistency) реализованы**; V2 state machine ещё не закрыт.

Согласованный порядок работ:

1. **Security:** закрыть передачу `WIFI_KNOWN.password` в Web model и
   WebSocket snapshot. Domain-модель и persistence не менять. **Сделано:**
   `ha-web::is_web_visible`, фильтрация в `web.rs` `drain`, `upsert`,
   `remove`, `snapshot_frames`; тест в `ha-web`. **Сделано.**
2. **Single control path:** Display запрашивает scan через
   `COMMAND_WIFI_SCAN`; Display и main читают `WIFI_SCAN`/`WIFI_STATUS`
   из Domain. Прямые C reads и локальный scan cache убрать.
   **Сделано:** local `WIFI_SCAN`/`take_scan` удалён, Display читает
   Domain, `Nav::WifiScan` постит Domain command.
3. **Known consistency:** Domain — canonical storage; C — runtime copy.
   Boot загружает Domain → C; изменения Domain ресинхронизируют C;
   удаление Domain удаляет C-запись. C → Domain legacy import убрать.
   **Сделано:** Domain subscription, `ha_wifi_known_remove`, полный
   reconcile C из Domain, legacy import удалён.
4. **V2 behavior:** state machine, IP-based connected, scan auth,
   scan clear on GOT_IP, known selection/full-list, backoff 16 s,
   start failure/restart.
5. **Verification:** host tests для pure selection/encoding/known sync/
   redaction; hardware scenarios — отдельно.

## 10. Решения, требующие согласования перед кодом

1. **Web redaction:** исключить `ENTITY_WIFI_KNOWN` из `MODEL`,
   `snapshot_frames` и любых диагностических Web-ответов; внутри Domain
   запись остаётся полной. Подтвердить, что Web вообще не должен видеть
   Wi-Fi credentials.
2. **Scan/status:** Display получает scan через Domain command и читает
   результаты/статус из Domain. Прямой `ha_wifi_status`/`ha_wifi_rssi` в
   Display/main удаляется. Подтвердить, что это единственный путь.
3. **Known sync:** Wi-Fi service подписывается на Domain
   `WIFI_KNOWN` facts и reconcile-ит C runtime, либо получает отдельный
   typed sync API. Предпочтение — Domain subscription без прямых
   service-to-service вызовов; нужен `ha_wifi_known_remove` в C.
4. **Forget:** удаление `WIFI_KNOWN` из Domain должно удалять C-запись.
   Новую пользовательскую команду forget не добавлять, пока её нет в V2;
   forget идёт через существующий Domain remove path.
5. **State machine источник:** C владеет radio state и GOT_IP status,
   Rust публикует canonical record. Подтвердить, что C отдаёт
   `IDLE/SCANNING/CONNECTING/CONNECTED/ERROR`, а не только association.
6. **SNTP:** перенос в System service — отдельный P2, не менять в P0.

Код не меняется до подтверждения пунктов 1–5.
