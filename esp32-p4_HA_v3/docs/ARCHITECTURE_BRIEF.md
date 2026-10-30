# Home Automation V3 — архитектурный бриф

Draft 0.2 · 24 октября 2026 · Концепция, не утверждённая спецификация
(0.2: зафиксирована аппаратная схема P4 Coordinator + C6 RCP)

> Целевой контракт и правила миграции: [TARGET_ARCHITECTURE.md](TARGET_ARCHITECTURE.md).
> Карта слоёв, владение runtime и порядок рефакторинга:
> [V3_LAYERS.md](V3_LAYERS.md).
> При расхождении по слоям/runtime действует `V3_LAYERS.md`, по canonical-контракту —
> `TARGET_ARCHITECTURE.md`.

## 1. Видение

Home Automation V3 (HA V3) — автономная система домашней автоматизации на ESP32-P4, преимущественно написанная на Rust. Основные свойства: модульность, низкие накладные расходы, локальная работа, переносимые библиотеки и широкий охват Zigbee-устройств за счёт самостоятельного Rust-движка совместимости, ориентированного на Zigbee2MQTT.

HA V3 не является портом Home Assistant. Не требуется запуск Zigbee2MQTT как приложения, MQTT-брокера или JavaScript-runtime на ESP32.

## 2. Цели

- Основная прикладная логика на Rust, включая async/await.
- Zigbee-модуль с независимым API, не привязанный к MQTT, UI, Domain Store или ESP32.
- Повторное использование моделей устройств, fingerprints и конвертерных подходов Zigbee2MQTT/ZHA с измеримым покрытием.
- ESP32-P4 выполняет прикладную логику, Zigbee Coordinator и весь Zigbee-стек; ESP32-C6 работает как RCP — только радио (решено, см. §4).
- Сохранить принципы V2: команды — намерения, события — факты, Domain Store — источник подтверждённого состояния, Domain Delta — уведомление о совершённом изменении.
- MicroDB — универсальное хранилище без знаний о Zigbee и UI.
- Возможность тестировать переносимое ядро на ПК без прошивки ESP32.
- Локальная работа без облачных зависимостей.

## 3. Вне рамок / не требуется

- Встроенный MQTT как транспорт Zigbee: MQTT в будущем реализуется отдельным независимым модулем.
- Перенос интерфейса, MQTT-обвязки или сервисов приложения Zigbee2MQTT.
- Запуск JS/Node.js на P4 для исполнения upstream-конвертеров.
- Переписывание всех уровней Zigbee (MAC/NWK/APS/security) на Rust в первой версии.
- Обещание 100% совместимости со всем каталогом Z2M после импорта fingerprints/exposes.

## 4. Аппаратная модель (зафиксировано)

```text
ESP32-P4: HA V3 (Rust) + Zigbee Coordinator
  ├─ Domain Store / Automation / UI / MicroDB
  ├─ Zigbee integration adapter (Rust)
  └─ Zigbee engine: interview, ZCL, catalog, converters
       └─ Espressif Zigbee SDK (C) через FFI
               │  Spinel, UART1, 460800
               │  P4 GPIO14(TX)→C6 GPIO20(RX)
               │  P4 GPIO15(RX)←C6 GPIO21(TX)
ESP32-C6: ot_rcp — только IEEE 802.15.4 радио
               │
       Zigbee devices
```

Схема уже работает в V2 и фиксируется для V3: координатор и весь Zigbee-стек
(esp-zigbee-lib/SDK) живут на P4, C6 прошит `ot_rcp` (голое 802.15.4-радио) и
общается по внутренней UART/Spinel; внешние перемычки не нужны. Отдельный
транспорт P4↔C6 не проектируем — используем существующий канал.

Следствие для Rust: собственный NWK/APS/security/ZDO не пишем. Rust-слой V3
работает поверх существующего SDK через FFI (interview, matching, ZCL-обвязка,
конвертеры, каталог), а host-тесты идут на fake-координаторе.

## 5. Логические модули

```text
ha-v3/
  crates/
    ha-domain/          # доменная модель, commands, подтверждённое состояние
    micro-db/           # универсальное embedded-хранилище (ram/flash, ring)
    z2m-rs/             # порт Z2M: profiles, converters, matcher, configure plans
    ha-automation/      # движок правил (порт V2 automation), host-тесты
    zigbee-devices/     # HA Zigbee Layer: interview, registry, canonical projection
    zigbee-exec/        # надёжная доставка запросов (TSN, retry, fake transport)
    zigbee-frames/      # ZCL-кадр для C→Rust границы
    ha-web/             # бинарный протокол UI (кадры, canonical records)
    ha-app/             # ESP32-P4 прошивка + C-мост native_zigbee (вне workspace)
  tools/
    zhc-compiler/       # pinned upstream import, генерация, отчёты
    z2m_spike/          # разовые аудиты и golden-фикстуры ZHC
```

### Границы модулей

- zigbee-core и zigbee-devices не знают о HA V3, Domain Store, WebSocket, MQTT.
- zigbee-transport определяет переносимые интерфейсы запросов, ответов и уведомлений координатора.
- ha-app/native_zigbee реализует платформенный транспорт/интеграцию SDK.
- domain принимает нормализованные Zigbee events и обновляет подтверждённое состояние через MicroDB.
- MQTT, если появится, подписывается на Domain Delta, а не используется как обязательная часть Zigbee.

## 6. Работа с устройством

1. Пользователь разрешает joining; Zigbee coordinator принимает устройство.
2. Zigbee engine выполняет interview: Basic cluster, endpoints, input/output clusters и дополнительные атрибуты по необходимости.
3. Поиск fingerprint выбирает кандидатов; endpoints/clusters уточняют совпадение.
4. Device definition указывает capability profile и protocol bindings/handlers.
5. Движок выполняет требуемую async-конфигурацию (bind, reporting, manufacturer-specific sequences).
6. События устройства преобразуются в нормализованные свойства/действия.
7. Адаптер HA V3 проводит их через Domain Store; успешная мутация создаёт Domain Delta.
8. Команды в Zigbee асинхронны; отправка команды не равна изменению подтверждённого состояния.

Для неизвестных устройств сохраняется диагностическая сигнатура; могут работать стандартные ZCL capabilities. recognized, supported, configured и online — разные состояния.

Удаление устройства (порт V2 `device_remove` + `zigbee_remove_mark`): сначала
durable tombstone в реестре, затем каскад canonical-записей (state, capabilities,
entity, logical device) и ZDO Mgmt_Leave. Если устройство вне сети, tombstone
остаётся; на announce leave повторяется, интервью не запускается. После
подтверждённого leave tombstone снимается. Group item каскад появится вместе с
Groups; автоматизации V2 при удалении тоже не каскадирует.

## 7. Модель конвертеров и каталог

Каталог не равен полному исполнению конвертеров.

- fingerprint → идентификация модели с учётом неоднозначностей.
- capability profile → доступные свойства, события и команды.
- protocol binding → какие кластеры, атрибуты, DP и handlers реализуют данные возможности.
- configure → нативные Rust async-последовательности или ограниченное декларативное описание.
- special converter → отдельный Rust handler для моделей с уникальным поведением.

Инструмент `tools/zb-catalog` загружает закреплённые версии upstream, извлекает переносимые определения, нормализует их, генерирует Rust-данные и отчёт по покрытию/конфликтам. Обычная firmware-сборка не требует сети. Исполняемый JS нельзя гарантированно транслировать в Rust автоматически. Для modernExtend и известных шаблонов можно наращивать покрытие правилами генератора и общими Rust-обработчиками.

Исходные сведения из предыдущей проверки проекта: zigbee-herdsman-converters@26.123.0, 4595 определений, 2588 fingerprints, 1083 уникальных exposes сигнатур; эти цифры — снимок конкретной версии, а не гарантия полного рабочего покрытия. Использование ZHA quirks как дополнительного источника декларативных Tuya DP/endpoints требует проверки на примерах. Лицензионные уведомления и источники фиксируются отдельно.

## 8. Store и поток состояния

Сохраняем правило V2:

```text
Command -> Zigbee async -> real report/event -> Domain mutation
        -> MicroDB -> Domain Delta -> WebSocket/UI + automation/journal
```

micro_db остается низкоуровневым универсальным storage engine (slot/index/generation/version, RAM/Flash политики, metadata check), не знает о Zigbee и доменной логике. Rust API/порт MicroDB V2 — отдельная задача; не объявляем уже реализованным. Подтвержденное состояние не меняется при одной лишь отправке команды. Одноразовые actions (например, short/double/hold) — события, а не постоянные состояния.

Статус V3: Domain остаётся RAM-кэшем, но прошивка пишет его canonical-записи в
MicroDB и восстанавливает при старте: реестр устройств — в разделе `mstore`,
entity/capabilities/property-state/groups/automation — в разделе `domain`
(write-through через `RecordSink` в HA Zigbee Layer). `EntityDesc.backing` пока
`Ram`; это осознанно, запись выполняет прикладной слой, а не сам Domain.

Группы и автоматизации хранятся в Domain как canonical-записи и переживают
reboot. `ha-automation` исполняет EVENT/STATE/TIME-триггеры: состояние и события
приходят из моста после подтверждённой мутации, условия читают Domain, действия
уходят в обычный CommandRouter/capability-gating. TIME-триггеры используют SNTP;
`Location`/`Settings` задают ручное смещение для локального времени (auto пока
UTC, GeoIP в V3 нет).

## 9. Производительность и ограничения

Для обработки ZCL, DP, fingerprints и async-команд ESP32-P4 ожидаемо достаточно по CPU; главным риском являются RAM, Flash, размеры очередей, число одновременно ожидающих операций и транспортный интерфейс C6. Декларативные таблицы могут находиться во Flash, в RAM держим состояния реально подключённых устройств. Используем bounded queues и детерминированное поведение при перегрузке. Целевые количества устройств и памяти утверждаются после прототипных измерений; оценки не считаются бенчмарком.

Rust execution stack для P4 требует отдельного выбора/проверки: esp-idf-svc/std и esp-hal/no_std + Embassy — разные схемы, не следует смешивать их без proof of concept. Сборка, зависимости ESP-IDF Zigbee SDK, Rust FFI и OTA также требуют прототипа.

## 10. План реализации

- **Этап 0** — фиксация V2: рабочий TS0041 и текущие решения сохраняются как regression fixture и эталон событий single/double/hold.
- **Этап 1** — прототип платформы: cargo на P4, async, C6↔P4 команды/ответы, Zigbee coordinator API, минимальные RAM/Flash метрики.
- **Этап 2** — переносимое ядро: interview, matching, ZCL, событийный API, fake coordinator, тестирование на ПК.
- **Этап 3** — конвертеры: стандартные on/off, battery, reporting; затем TS0041 и распространённые Tuya DP; полноценная round-trip проверка.
- **Этап 4** — генератор: upstream pins, parse fingerprints/exposes/known extensions, dedup и Rust таблицы, отчёт по covered/partial/recognized-only/unsupported.
- **Этап 5** — HA-интеграция: Domain Store + MicroDB, команды, подтверждённые события и Domain Delta, Web UI/automation.
- **Этап 6** — расширение: Aqara/Sonoff/Tuya, специализированные handlers, обновления каталога, тесты на физических устройствах.

## 11. Критерии успеха

- Один Zigbee engine работает в host-тестах и на P4 без знаний о UI/MQTT.
- TS0041 проходит join/interview/configure и корректно генерирует single/double/hold.
- Generic ZCL и хотя бы один Tuya DP сценарий работают end-to-end.
- Команда не изменяет доменное состояние до получения подтверждённого сообщения.
- Отчёт генератора различает распознавание и реальную поддержку, включая причины отказа/конфликты.
- Замерены RAM, Flash, очереди, latency и восстановление после переподключения C6.

## 12. Открытые решения

1. ~~C6 — полноценный coordinator с SDK или RCP~~ — решено: P4 — координатор и стек, C6 — `ot_rcp` (802.15.4) по UART/Spinel (§4).
2. Rust на P4 — ESP-IDF/std или no_std/Embassy, интеграция с FFI.
   PoC: ESP-IDF/std (esp-idf-svc 0.53, nightly-2026-07-08) — работает (§14);
   окончательно фиксируем после FFI к Zigbee SDK.
3. Формат транспорта P4↔C6 и его гарантии при рестарте/потере пакетов.
4. Типовая схема значений/capabilities для domain и конвертеров.
5. Rust-обвязка существующей MicroDB или её переосмысление как отдельного Rust crate.
6. Политика импортирования и лицензирования Z2M/ZHA, версии и обновления обработчиков.
7. Определение количественных целей: устройств, бюджет памяти, покрытие моделей.

## 13. Архитектурное правило

Zigbee — автономный модуль протокола и устройств. Domain Store — источник подтверждённого состояния. MQTT — возможная внешняя интеграция, никогда не фундамент работы Zigbee.

## 14. Проверенный стек (PoC, 24.10.2026)

Официальный шаблон `esp-rs/esp-idf-template` собран и запущен на плате B
(P4 rev v1.3, C6 `ot_rcp`), USB-консоль:

```text
I (179) zigbee_esp: Hello, world!
```

- Rust: **nightly-2026-07-08** + `rust-src`, `riscv32imafc-esp-espidf`, `build-std`
  (свежий nightly ломает std для espidf: `libc::stat` без `st_atime`).
- ESP-IDF: **v6.1** (managed, esp-idf-sys 0.38.1), esp-idf-svc 0.53.
- Обязательно `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`: rev v1.3 — pre-v3.0,
  нестандартный CLIC; без флага std падает в `Illegal instruction`.
- Консоль — `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`.
- Windows: `libclang` для bindgen (`LIBCLANG_PATH`), `target-dir` вне глубокого
  пути (ограничение esp-idf-sys).
- Проект: `crates/ha-app` (C-мост — `native_zigbee`); сборка — `docs/BUILD.md`.

Следующий шаг PoC: FFI к `esp-zigbee-lib` (P4 coordinator, UART/Spinel к C6),
C-callback → bounded queue → Rust, событие TS0041.

## 15. Независимые компоненты и единый протокол HA

HA V3 сохраняет фундаментальный принцип V1/V2: **единая каноническая модель
данных с единым бинарным представлением**. Domain, MicroDB, Automation,
WebSocket, UART и LVGL используют одни типы и записи без промежуточных DTO,
повторной сериализации и дублирования интерпретации. Транспорты добавляют только
собственный framing/целостность, payload остаётся общим.

Протокол типов — фундамент интеграции HA, а не обязательство для переносимых
библиотек. Независимые компоненты не зависят от него и не импортируют типы HA:

- **`micro-db`** — универсальное хранилище, не знает, что хранит;
- **`z2m-rs`** — порт системы совместимости Zigbee2MQTT: fingerprints, profiles,
  converters, Tuya DP, CommandMap, configure plans. Не зависит от Domain,
  MicroDB, UI, ESP-IDF и протокола HA; пригоден вне этого проекта;
- **`ha-app/native_zigbee`** — транспорт Espressif SDK (C FFI, Spinel/UART,
  coordinator); бывший PoC-крейт `zigbee-esp` теперь часть прошивки;
- **HA Zigbee Layer** (`zigbee-devices`) — единственный адаптер, который знает обе
  стороны: `z2m-rs` и канонические типы HA. Здесь происходит преобразование
  `DeviceEvent/Action` → `PropertyChanged/Action`, и только здесь.

Правило: один канонический формат внутри HA, минимальные преобразования на
границах независимых компонентов, отсутствие искусственной связанности.

Границы порта Z2M:

- переносим **систему совместимости**, а не JS-рантайм: данные профилей +
  универсальный Rust-движок;
- `modernExtend`/`extends` раскрываются генератором до сборки, в прошивке нет
  наследования;
- уникальная логика — ручные Rust-handlers, а не отброшенные правила;
- вендорская специфика (Tuya DP и др.) — вариант `Converter`/codec, а не
  отдельная подсистема; архитектура одна для всех производителей;
- `recognized ≠ supported`: отчёт генератора обязан различать распознанную
  модель и модель с рабочими конвертерами; невыразимые JS-функции перечисляются
  явно, молчаливое отбрасывание запрещено;
- MQTT в порт не входит.

Первые производители: Tuya, Aqara, Sonoff. Ориентир flash под каталог — 256 КБ,
окончательный бюджет — по измерениям. Configure с самого начала проектируется как
state machine с корреляцией seq, таймаутами и повторами; первая реализация —
bind/read/reporting.

`ha-data` отдельно не создаётся: общие типы выделяются только когда появляется
реальная вторая зависимость внутри HA.


