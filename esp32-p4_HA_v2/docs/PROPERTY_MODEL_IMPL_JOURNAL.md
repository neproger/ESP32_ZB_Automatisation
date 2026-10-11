# Семантическая модель — журнал миграции

> Журнал ведётся по мере работы. Контракт — `PROPERTY_MODEL.md`.
> Здесь: статус, план по фазам с чекпоинтами, решения и хронология.

## 1. Статус

| | |
|---|---|
| Компонент | `ha_model` (словарь) + модуль-мост (ZCL ↔ семантика) |
| Контракт | `PROPERTY_MODEL.md` |
| Стратегия | вариант B: семантика без смены canonical identity и storage geometry |
| Реализовано | фаза 0–2: словарь + мост `semantics`; Automation и Display (`ui_widgets`, `ui_status_bar`) работают через семантику |
| Миграция данных | **не требуется** до шага A |
| Проверено на P4 | — (поведение не менялось; IDF-сборка зелёная) |

## 2. Принципы миграции (почему безопасно)

1. **Формат хранения не меняется** до шага A. Ни `sizeof`, ни форма ключа → `mstore` не
   видит смены геометрии → erase не нужен (`RECORD_MODEL.md` §3).
2. **Каждая фаза аддитивна или локальна.** Новая семантика появляется рядом; потребители
   переключаются по одному, с хост-тестом на поведение.
3. **`property_id` не хранится** в персистентных записях: `resolve(cluster/attr → property)`
   — во время выполнения. Хранение property в записи = миграция, поэтому пока запрещено.
4. **Откат — revert коммита.** Пока формат не менялся, откат фазы не оставляет мусора в
   flash.
5. **Домен/провод не трогаем** до A. Если фазе нужен аддитивный байт — только через
   `reserved` существующей записи, с `static_assert`, и отдельным решением в §4.
6. **Physical ref — opaque address.** Потребитель носит `(uid/ep/cluster/attr)` как адрес,
   но не ветвится по `cluster/attr/command`; смысл даёт только мост.

## 3. План по фазам (чекпоинты)

### Фаза 0 — словарь и дескрипторы (`ha_model`, аддитивно)

Цель: завести `ha_property_id_t`, `ha_action_id_t`, `ha_event_id_t` и
`ha_property_desc_t` (имя, `value_kind`, `unit`, `range`, `flags`) — **без ZCL**.

- [x] `ha_model/include/ha_model/ha_properties.h`: enum'ы и дескриптор (+ `ha_property_desc(id)`)
- [x] `ha_value_t` (kind + фиксированный union) — единый runtime-результат (см. `PROPERTY_MODEL.md` §4.4)
- [x] таблица дескрипторов для известных свойств; `UNKNOWN` как честная деградация
- [x] host-тест: lookup дескриптора, содержимое, дефолт `UNKNOWN`, невалидный id
- [x] НИЧЕГО не подключаем к существующим путям (поведение системы не меняется)

**Чекпоинт 0.** Сборка IDF + host-тест зелёные; на P4 поведение идентично. Откат — revert.
Риск: нет.

### Фаза 1 — мост: декодирование state (ZCL → семантика)

Цель: единственное место `cluster/attr → property` и декодирования значения.

- [x] компонент-мост `semantics` (линкуют `zigbee` и потребители; Domain — нет)
- [x] приватная таблица `zb_property_map_t[]`: (cluster, attr) → property, expected_type, decode_kind
- [x] публичный API: `semantics_property_from_key(key)`, `semantics_state_value(key, state) → ha_value_t`
- [x] `attr_scale` перенесён в таблицу как `ZB_DECODE_SCALE`; добавлены нелинейные стратегии
      (`ILLUMINANCE`, `MIREDS_TO_KELVIN`, `BITMAP_BOOL`)
- [x] host-тест: паритет (temperature/humidity/battery V/pct/OnOff) **и отдельно**
      нормализация (illuminance, mired→K, bitmap→bool) + отказы (нет маппинга, тип вне словаря)

**Чекпоинт 1.** Host-тест зелёный; IDF-сборка (компонент `semantics`) зелёная. Мост ещё
никем не подключён, поэтому поведение прошивки не менялось. Откат — revert. Риск: расхождение
масштаба — покрыт тестом паритета.

### Фаза 2 — потребители state на property

Цель: снять `cluster/attr`-свитчи у потребителей; потребитель адресует семантику и не
конструирует ZCL-ключи. Уточнено ревью: Display строит соседние ключи (`apply_color`),
поэтому нужен обратный API моста (2.0) — одной замены `switch` недостаточно.

- [x] **2.0** `semantics_property_key(context, property, out)` — обратный маппинг в том же объекте
- [x] **2.1** расширен state-mapping: `BRIGHTNESS, COLOR_HUE, COLOR_SATURATION, COLOR_X, COLOR_Y,
      COLOR_TEMPERATURE, POWER, OCCUPANCY, TEMPERATURE, HUMIDITY, ILLUMINANCE, BATTERY_*`;
      сверх плана добавлены system-свойства (нужны для `HAS_BITS` по `WEEKDAY_MASK`) + `semantics_value_to_double()`
- [x] **2.2** Automation: `automation_rule_state_value()`/`attr_scale()` удалены, `condition_ok` и
      STATE-триггер декодируют через `semantics_state_value()`; `ha_zigbee.h` убран из чистой
      логики правила. Сравнение `rule.cluster/attr == key.cluster/attr` в matching осталось —
      это сравнение **opaque identity**, не семантика. Host-тесты переведены на реальные пары
- [x] **2.3** Display: `ui_widget_kind_for(property)`; формат по `ha_value_t` + единице
      дескриптора; switch/indicator — по булеву значению; слайдеры в семантике (level/яркость
      — %, цветовая температура — K); ZCL-аргументы команды конвертируются на границе отправки
- [x] **2.4** Display: `apply_color` берёт X/Y/яркость через `semantics_property_key()` +
      `semantics_state_value()`; `ui_status_bar` (часы) читает system-свойства через мост —
      ручных ZCL-ключей в UI не осталось
- [x] **2.5** IAS: **вывод исследования.** `ZONE_STATE` (0x0500/0x0000) — это enum статуса
      enrollment (0/1), **не** тревога; тревога — битовое `ZoneStatus` (0x0002, bit0=Alarm1…).
      Прежний маппинг `ZONE_STATE → «Тревога»` был семантически неверен. `HA_PROPERTY_ALARM`
      пока **не вводим**: сначала решить семантику (enrollment vs alarm1/2 + tamper/battery)
      под реальное устройство. IAS-виджеты сейчас рендерятся как `VALUE` (нет маппинга)

**Чекпоинт 2.** Host-тесты зелёные; на P4: правило срабатывает, виджет рисуется. Откат —
revert. Риск: неполный маппинг — `UNKNOWN` деградирует явно, без ложной семантики.

### Фаза 3 — команды: Property + Action + value

Уточнено перед реализацией: команда не всегда один скаляр (`MoveToColor` = X и Y), поэтому
semantic request и физическая команда разделены, а `COLOR SET(x, y)` — отдельная capability.

- [x] **3.0** контракт: `ha_command_value_t` (`SCALAR`/`XY`), `HA_PROPERTY_COLOR` (командная
      capability); `COLOR_X`/`COLOR_Y` остаются state-свойствами
- [x] **3.1** мост: `semantics_build_command(target, property, action, value) → ha_zb_command_t`;
      приватная `zb_action_map` + кодировщики (`scale`/`transition`/`direction`/LE)
- [x] **3.2** Display → `display_send_command()` (семантический); `display_send_onoff/level/
      hue/saturation/color_xy/color_temperature` удалены
- [x] **3.3** из Display убрано кодирование (`%→254`, `K→mired`, LE): Display шлёт %, °, K, xy
- [x] **3.4** Automation execution: сохранённая физическая action-запись остаётся **opaque**
      (`automation_rule_command` копирует без интерпретации); перекодировка — шаг A
- [x] **3.5** tests: семантический запрос → та же `ha_zb_command_t`, что раньше
      (OnOff/Level/MoveToColor/…); host `test_semantics` 1/1

**Чекпоинт 3.** На P4: вкл/выкл/уровень/цвет работают, как раньше (те же кадры). Откат —
revert. Риск: кодирование args (level 3 байта и т.п.) — покрыть тестом паритета кадров.

### Фаза 4 — события: HA_EVENT_*

Цель: сырые ZCL-события устройства → семантические id.

- [ ] `ha_event_id_t` (SINGLE_PRESS/DOUBLE_PRESS/HOLD/...)
- [ ] маппер сырого события → `HA_EVENT_*`: допускает **профиль устройства (device/model)
      + payload-декодер**, не только одну статическую таблицу `cluster/command`
- [ ] Automation `DEVICE_EVENT` триггер работает по семантическому событию (runtime)

**Чекпоинт 4.** На P4: правило на нажатие кнопки срабатывает. Откат — revert. Риск: у
разных устройств разный способ слать кнопки — `UNKNOWN` допустим.

### Фаза 5 — Web-адаптер и фронтенд

Цель: UI получает семантику; фронт теряет ZCL-словарь.

- [ ] Web BFF отдаёт `property`/`value`/`unit` (аддитивно к сырому состоянию)
- [ ] фронт: `zcl.js`/`capabilities.js`/`commands.js`/`automation.js` — на семантику
- [ ] удалить независимые ZCL-таблицы из фронта (единый источник — сервер/мост)

**Чекпоинт 5.** web-ui собирается; UI показывает имена/единицы без `cluster/attr`;
провода/формы работают. Откат — revert. Риск: объём фронта — фаза дробится на под-шаги.

### Фаза 4 — события: физическая форма → семантика

Ключевое решение: событие эфемерно, поэтому нормализуем **до** публикации в Domain. Не строим
фазу вокруг текущего `command_id`.

- [x] **4.0** физическая форма `ha_zb_event_t` (device/endpoint/cluster/command/payload) —
      Zigbee её уже несёт целиком (`zigbee_event_t`), cluster/payload не теряются
- [x] **4.1** `semantics_decode_event(physical, device_profile, out) → ha_event_t`; общий
      `ha_event_id_t` (вкл. `HA_EVENT_MINUTE_TICK`); host-тесты по парам/профилю
- [x] **4.2** Zigbee: `semantics_decode_event` → публикует EVENT `value = HA_EVENT_*` + сырой
      `ha_zb_event_t` в payload (ВРЕМЕННЫЙ shim)
- [x] **4.3** System: события переведены на общий `ha_event_id_t` (`HA_EVENT_MINUTE_TICK` и др.);
      `ha_system_event_t` удалён из контракта; web-ui `SYSTEM_EVENTS` → id 4..7
- [x] **4.4** Automation: событие читает `event_id` из `value`; TIME-триггер — по
      `HA_EVENT_MINUTE_TICK`; новый путь адресует semantic id
- [x] **4.5** legacy DEVICE_EVENT: `match_command` = сырой `command_id` из transient payload
      (shim); перевод на semantic id — шаг A. **Удалить shim на шаге A** вместе с полем
      `event_id` в `ha_automation_record_t`
- [x] **4.6** host-тесты `semantics_decode_event` (пары/профиль/NULL); host `test_rule`/`test_semantics`
      1/1; IDF зелёная; прошито на P4

Итог: **event plane transport-agnostic** — `DOMAIN_FACT_EVENT.value` всегда `HA_EVENT_*`,
источник (Zigbee/System) в id не зашит. State identity и automation storage — ещё transitional.

### Фаза 5 — Web/UI: семантический слой аддитивно (WS v3 не нужен)

Цель: **после Фазы 5 web-ui не импортирует и не интерпретирует cluster/attr/ZCL command ids в
обычном HA-потоке.** Raw Zigbee остаётся **внутренним ABI Web↔Domain** до шага A. Делаем
аддитивно поверх существующего протокола (новые message types / BFF-структуры), WS v3 не
поднимаем — он нужен только при смене canonical state key на шаге A.

- [x] **5.0 core** в `semantics`: `semantics_cluster_properties()` / `semantics_property_actions()`
      — capabilities без inference в браузере; host-тесты
- [x] **5.0 BFF** проекция state через `semantics` + общий wire-кодек semantic-value
      (`bits_to_value`/`value_to_bits`); capabilities endpoint — пока через 5.0 core
- [x] **5.1** аддитивный `WEB_MSG_SEMANTIC_STATE` (0x12) / `_REMOVE` (0x13): DTO
      `(uid, ep, property, kind, bits)`, без cluster/attr/zcl_type/raw; snapshot+delta проекция
      (UNKNOWN/decode-fail → не шлём); browser store по ключу `(uid, ep, property)`;
      `EndpointWidgets` читает семантику, а не raw-скейлы
- [x] **5.2** command ingress: `WEB_CMD_SEMANTIC_COMMAND` (стабильный LE DTO, без C enum/union);
      Web декодирует, валидирует (размер/форма/NaN-Inf) → `semantics_build_command()` →
      `HA_CMD_ZIGBEE_CLUSTER`. `web_do_zb_command()` оставлен transitional. Живые контролы
      (`EndpointWidgets`, `StateAttr`) отправляют семантику; `commands.js` больше не кодирует ZCL
- [x] **5.3.0–5.3.3** semantic-правило: модель `ha_sem_rule_t` (`ha_automation.h`); компилятор в
      `semantics` (`semantics_compile_automation`) + reverse `semantics_event_to_physical`;
      `WEB_CMD_SEMANTIC_AUTOMATION_PUT` (LE DTO) → сначала в 144-байтную запись, потом в Domain
- [x] **5.3.6** host-тест: semantic rule → точная физическая запись (temp>25→POWER ON; POWER==1
      +temp>25→BRIGHTNESS 70%; TIME 07:30→POWER ON; system MINUTE_TICK→cmd=event id); невыразимое
      (неоднозначный event, BETWEEN на STATE-триггере) → отказ
- [x] **5.3.4a** обратная проекция `semantics_decompile_automation` (physical → semantic) + host-тест;
      невыразимое правило → `false` (Web/UI покажет как legacy/raw, не выдумывая смысл)
- [x] **5.3.4b** semantic automation DTO: `WEB_MSG_SEMANTIC_AUTOMATION` (0x14) / `_REMOVE` (0x15),
      общий rule-кодек (read=write), snapshot+delta; `representable=0` для legacy/невыразимого;
      browser store `semAutomations` по id; raw `ENTITY AUTOMATION` без изменений
- [x] **5.3.4c/d** frontend переведён на semantic rule: `automation.js` — только vocabulary/описания
      (нет `ACTION_CLUSTERS`/`TRIGGER_CMDS`/`buildActionArgs`/`decodeActionArgs`); `proto.semanticAutomationPut`;
      `AutomationForm`/`Automations` читают `store.semAutomations`, селекторы — из semantic states/capabilities;
      `representable=0` → «legacy / непредставимо», без открытия в редакторе
- [x] **5.3.4-snapfix** профили устройств для reverse-event собираются вне automation-итерации
      (кэш в snapshot) — snapshot и delta дают одинаковый `representable`
- [x] **5.3.5** старый `WEB_CMD_AUTOMATION_PUT` оставлен transitional
- [x] **5.4** Events: `WEB_MSG_EVENT` (0x16, стабильный LE: source_kind/source_uid/endpoint/
      event_id/value); Web сериализует уже семантический `DOMAIN_FACT_EVENT` (raw `ha_zb_event_t`
      наружу не отдаётся); Events-страница на `eventName()`, без cluster/command_id/`SYSTEM_EVENTS`
- [x] **5.5.0** endpoint semantic capabilities BFF: `WEB_MSG_SEMANTIC_CAPABILITIES` (0x17)/`_REMOVE`
      (0x18), DTO (uid,ep,properties[{property,actions}]) из `semantics_cluster_capabilities`
      (server-кластеры); snapshot+delta
- [x] **5.5.1** `EndpointWidgets` строится из capabilities (property+actions), а не из
      `endpoint.clusters`+`commands.js`
- [x] **5.5.2** semantic Group Item facade: `WEB_MSG_SEMANTIC_GROUP_ITEM` (0x19)/`_REMOVE` (0x1A)
      + `WEB_CMD_SEMANTIC_GROUP_ITEM_PUT` (16)/`_REMOVE` (17); persistent `group_item` остаётся
      raw, BFF проецирует raw↔property (UNKNOWN не публикуем); `Groups.jsx` — только
      (uid,ep,property) + `semState`/`propertyName`
- [x] **5.5.3** SystemStatus/DeviceCard/DeviceDetail → semantic states (`semState`) и vocabulary;
      обычные страницы не читают raw state map/cluster
- [x] **5.5.4** raw Zigbee (cluster/attr/zcl_type/raw, endpoint profile) → отдельный
      `Diagnostics`-экран (читает существующий raw store, без нового backend protocol)
- [x] **5.5.5** удалено мёртвое: `zbCommand`, `automationPut`, `encodeAutomationRecord`,
      raw `groupItemPut`/`groupItemRemove` (front), `store.automations`, `capabilities.js`,
      `commands.js`, raw `SYS_ATTR`/`CLUSTER_SYSTEM` в `system.js`
- [x] **5.5.6** `zcl.js` — только `uidHex`/`hex16` (общие) + ZCL-хелперы для Diagnostics;
      обычные страницы их не используют
- [x] **5.5.7** grep-чек: в обычных pages/components ZCL отсутствует; ZCL — только
      `Diagnostics.jsx`, `StateAttr.jsx`, `schema.js` (transport decoder), `zcl.js`

**Чекпоинт 5.** 5.0 (core+BFF), 5.1, 5.2 готовы. Проверено на P4: semantic command ingress
(POWER/BRIGHTNESS/COLOR_TEMP/COLOR доходят, toggle переключает) и read-path — `WEB_MSG_SEMANTIC_STATE`
отдаёт `(uid, ep, property, value)` в человеческих единицах (system time, POWER, BRIGHTNESS=70.08,
COLOR_X/Y=0.3, TEMP). Осталось: 5.3 (Automation UI-компилятор), 5.4 (Events), 5.5 (удаление
ZCL-семантики из фронта); raw `ENTITY` остаётся для диагностики до 5.5/A.

### Фаза A (позже, отдельный трек)

Не входит в B. Начинается при появлении второго транспорта.

- [ ] смена canonical identity на `(entity_id, property_id)`
- [ ] erase затронутых FLASH-таблиц + WS v3 (`WEB_PROTOCOL.md`)
- [ ] обновление `ARCHITECTURE.md`, `RECORD_MODEL.md`, `docs/services/*`
- [ ] потребители не меняются — они уже на `Property/Action/Event`

## 4. Решения (decision log)

| Дата | Решение | Почему |
|---|---|---|
| 2026-10-10 | Выбран вариант B: семантический слой без смены canonical identity | Domain уже абстрактный; протечка выше; A требует erase + WS v3 ради одного транспорта |
| 2026-10-10 | `scale`/`signed`/`offset` — в транспортном маппинге, не в общем дескрипторе | это свойство ZCL-кодировки, а не свойства вообще; у Matter будет иначе |
| 2026-10-10 | Обобщаем не только state, но и Action/Event | иначе половина протечки остаётся в командах/событиях |
| 2026-10-10 | `property_id` не хранится в персистентных записях (resolve в рантайме) | иначе миграция storage уже на фазе B |
| 2026-10-10 | Маппинг ZCL ↔ семантика — в отдельном модуле-мосте, не в `zigbee` и не в `ha_model` | потребители не должны зависеть от сервиса `zigbee` (`ARCHITECTURE.md` §1.4) |
| 2026-10-10 | `zb_property_map`: `expected_type` optional, декодирование по `state->zcl_type`, `signedness` не вводится | тип уже приходит в записи; устройство может прислать другой допустимый тип — его нельзя прочитать ложно |
| 2026-10-10 | Единый runtime-результат — `ha_value_t` (kind + union) | иначе Automation/Display/Web заведут свои представления семантики |
| 2026-10-10 | Command path фазы 3: мост → `ha_zb_command_t` → существующий `HA_CMD_ZIGBEE_CLUSTER` | не трогаем Domain и маршрутизацию команд |
| 2026-10-10 | Action: target `(device_uid, endpoint)` задаёт вызывающий; property-mapping задаёт только команду | свойство = «что», адресат = «где» |
| 2026-10-10 | Event mapper допускает профиль устройства + payload-декодер, не одну статическую таблицу | кнопки разных вендоров кодируют press/hold/double по-разному |
| 2026-10-10 | Инвариант B: physical ref носится как opaque address, ветвиться по cluster/attr/command нельзя | до A identity физически протекает, но смысл — нет |
| 2026-10-10 | `*_id_t` — явные стабильные числа; удалённые значения не переиспользуются | semantic ABI: позже попадут в API Automation/Web и в canonical key |
| 2026-10-10 | `ha_property_desc()` никогда не возвращает NULL: неизвестный/невалидный id → дескриптор `UNKNOWN` | честная деградация без ложной семантики; потребителю не нужен NULL-чек |
| 2026-10-10 | `ha_model` стал компилируемым (добавлен `src/ha_properties.c`) | чистому lookup нужно определение; по-прежнему не линкует Domain |
| 2026-10-10 | `ha_value_t` НЕ объединяем с `domain_value_t` на фазе 0 | сходство структуры ≠ одинаковый контракт; отдельный открытый вопрос |
| 2026-10-10 | Мост хранит явную стратегию декодирования (`decode_kind`), `scale/offset` — лишь частный случай `ZB_DECODE_SCALE` | транспортное представление не всегда линейно (illuminance, mired→K, bitmap→bool); иначе мост ломается на первом же нелинейном свойстве |
| 2026-10-10 | Фаза 1 = только мост + host-тест; Automation/Display НЕ подключаются | паритет проверяем тестом, поведение прошивки не меняется |
| 2026-10-10 | Компонент-мост назван `semantics` (каталог `semantics/`), публичный API — только семантический | таблица (cluster, attr) и `decode_kind` приватны; потребитель видит лишь physical ref → property/value |
| 2026-10-10 | Тесты разделены: паритет (линейное, существующее) и нормализация (нелинейное) — разные функции | не смешивать миграцию архитектуры с исправлением старой математики |
| 2026-10-10 | В таблицу фазы 1 добавлены только: OnOff, temperature, humidity, battery V/pct, illuminance, color temp, occupancy | brightness/hue/sat/xy и system — по факту необходимости; словарь не раздуваем |
| 2026-10-10 | Мост получил обратный API `semantics_property_key()` и `semantics_value_to_double()` | потребитель не конструирует соседние ZCL-ключи; сравнение значений — через единый числовой вид |
| 2026-10-10 | Level→`BRIGHTNESS` (процент 0..100), ZCL hue→градусы, xy→0..1, color temp→Kelvin | семантические значения, а не raw; Display примет их в 2.3 |
| 2026-10-10 | System-свойства добавлены в mapping (нужны `HAS_BITS` по `WEEKDAY_MASK` и system-условия) | «по факту необходимости» наступил на 2.2 |
| 2026-10-10 | Условие на пару без маппинга → false | честная деградация; UI должен предлагать только замапленные свойства |
| 2026-10-10 | Display: форма виджета и формат — по свойству/единице; слайдеры в семантике (level %, цветовая температура K) | UI перестаёт знать ZCL; ZCL-аргументы команды конвертируются на границе (command-path — фаза 3) |
| 2026-10-10 | Display: соседние состояния (`apply_color`, часы) — через `semantics_property_key()` | потребитель не конструирует ZCL-ключи; ручных `cluster/attr` в UI нет |
| 2026-10-10 | IAS: `ZONE_STATE` — enrollment, тревога — `ZoneStatus`; `HA_PROPERTY_ALARM` не введён | не тащить неверную семантику; решать под реальное устройство |
| 2026-10-10 | Команды: `COLOR SET(x,y)` — отдельная capability; `COLOR_X/COLOR_Y` — state | не сводить все команды к одному скаляру; args[] наружу не тащить |
| 2026-10-10 | Запись Automation (action) остаётся opaque physical; мост её не перекодирует на fire | не платить CPU за `ZCL→semantic→ZCL`; ZCL↔semantic записи — шаг A |
| 2026-10-10 | Display не кодирует ZCL (%, °, K, xy наружу); transition/direction/LE — в мосте | потребитель не знает кодировки |
| 2026-10-10 | События нормализуем **до** публикации (эфемерны); `ha_event_id_t` — общий vocabulary всей системы | нет persistent-ABI, который держал бы ZCL; не строить вокруг `command_id` |
| 2026-10-10 | Legacy DEVICE_EVENT-запись (device+command, без cluster/payload) — transitional | lossless-переход к semantic event id невозможен; перевод — шаг A |
| 2026-10-10 | `DOMAIN_FACT_EVENT.value` всегда `HA_EVENT_*`; источник в id не кодируется | одно пространство event ids; источник есть отдельно (device) |
| 2026-10-10 | Сырой `ha_zb_event_t` в payload — только временный shim для legacy-правил | не второй публичный контракт; удаляется на шаге A |
| 2026-10-10 | System-события унифицированы в `ha_event_id_t` (сменились id) | одна семантика событий; цена — правка Automation + web-ui |
| 2026-10-10 | Фаза 5 — аддитивно поверх raw WS; WS v3 отложен до шага A | raw остаётся внутренним ABI Web↔Domain; не поднимать версию раньше смены key |
| 2026-10-10 | Capabilities (cluster→properties, property→actions) — в `semantics`, не в браузере | убрать ZCL-inference UI (`capabilities.js`) |
| 2026-10-10 | Semantic command DTO — стабильный LE (без C enum/union/`memcpy`), NaN/Inf reject на границе WS | wire не зависит от ABI компилятора; WS — trust boundary |
| 2026-10-10 | Transition policy (0) остаётся в мосте, не в DTO | нет потребности в configurable transition; ZCL-параметр не тащим наверх |
| 2026-10-10 | Semantic state — отдельный аддитивный message (0x12/0x13), не изменение raw `ENTITY STATE` | raw остаётся диагностикой/совместимостью; WS v3 не нужен |
| 2026-10-10 | Identity semantic state в store — `(uid, ep, property)`, не raw key | уже сейчас совпадает с будущей canonical identity шага A |
| 2026-10-10 | unit/range/name не дублируем в каждом state-кадре | descriptor — отдельно (`semantics.js`/vocabulary), в кадре только address+property+typed value |
| 2026-10-10 | Общий codec semantic-value (kind+bits) для command и state | одна реализация, без дублей |
| 2026-10-10 | Компиляция semantic-правила в 144-байтную запись — в мосте (`semantics_compile_automation`), не в JS | frontend не знает offsets/args/cluster/command id; host-testable |
| 2026-10-10 | EVENT компилируется только если reverse однозначен (`semantics_event_to_physical`); иначе отказ | не сохранять неверное правило; system-события — напрямую event id |
| 2026-10-10 | BETWEEN на STATE-триггере → отказ компиляции | в legacy-записи триггера нет второго порога |
| 2026-10-10 | Симметрия: `semantics_decompile_automation` (physical → semantic) для read-path UI | иначе frontend перестал бы писать ZCL, но продолжал его читать/понимать |
| 2026-10-10 | Невыразимое правило (неизвестный event/action, transition≠0) → `false`, UI показывает legacy/raw | не выдумывать смысл там, где reverse неоднозначен |
| 2026-10-10 | Rule-wire — один кодек для PUT и STATE; state DTO = id + representable + rule | read/write формы не разъедутся |
| 2026-10-10 | В snapshot (внутри `domain_entity_iter`) НЕ звать `domain_entity_get` | локи Domain реентерабельно не берутся: был дедлок web-задачи |
| 2026-10-10 | Профили устройств для reverse-event — кэш, собранный ОТДЕЛЬНЫМ iter DEVICE перед automation-iter | иначе snapshot (profile=NULL) и delta давали разный `representable` |
| 2026-10-10 | Automation UI — только `store.semAutomations`; raw `ENTITY AUTOMATION` не используется | иначе ZCL снова протёк бы в форму/описания |
| 2026-10-10 | Web сериализует уже семантический EVENT (не вызывает `semantics_decode_event` повторно) | иначе второй decoder; raw `ha_zb_event_t` — private transitional |
| 2026-10-10 | Events UI: единый `eventName()`; `SYSTEM_EVENTS` удалён | одно пространство event ids |
| 2026-10-10 | Capabilities endpoint'а — отдельный аддитивный message (0x17/0x18); без строк/units | UI строит контролы из семантики, не из clusters; имена/units — vocabulary UI |
| 2026-10-10 | `group_item` хранится raw до A, но consumer-контракт — semantic (facade) | не мигрировать storage зря; consumer уже semantic |
| 2026-10-10 | Group item с UNKNOWN property не публикуется в semantic stream | UI всё равно нечего с ним делать; raw остаётся в Diagnostics |
| 2026-10-10 | raw Zigbee — только на Diagnostics-экране (без нового backend protocol) | raw ENTITY уже приходит; это допустимое применение transitional ABI |
| 2026-10-10 | Frontend dead paths удалены (zbCommand/automationPut/encodeAutomationRecord/groupItem raw/capabilities.js/commands.js) | обычный UI — клиент semantic/domain API; ZCL только в Diagnostics |
| 2026-10-10 | Перед шагом A проведён архитектурный аудит ветки (`da30b93`); план — `STEP_A_PLAN.md` | A меняет identity/persistence/wire — необратимо; сначала проверка |
| 2026-10-10 | Шаг A: `entity_id` (opaque u64, derivation из binding) + **Entity/Binding split**; erase несовместимого persistent; новый minimal WS v3; Diagnostics вне v3; system/weather — entities, location/settings — config | §5 `STEP_A_PLAN.md` закрыт; открыт только точный алгоритм derivation (A0.3) |
| 2026-10-10 | A0.1: Entity — только logical identity (`ha_entity_id_t`+key), без транспорта; record минимален | Domain не знает uid/endpoint/cluster/attr; binding — adapter-owned (A0.2) |
| 2026-10-10 | A0.1 не задаёт вывод `entity_id` | derivation (seed→id) — A0.3; форма модели не должна зависеть от hash |
| 2026-10-10 | A0.2 binding — Zigbee-private `{ entity_id, device_uid, endpoint }`, без поля `transport` | таблица Zigbee-specific; transport не тащим в binding |
| 2026-10-10 | seed для A0.3 — канонические байты `[uid LE(8) | endpoint(1)]`, issuer (не transport) | нельзя `hash(&struct)` (padding/endian); cluster/attr в seed не входят (это Property) |
| 2026-10-10 | Derivation: `SipHash-2-4(fixed project key, LE32(issuer)||LE32(len)||seed)`, `0→1` | стабильный u64 из короткого seed; fixed key → reboot-stable; не CRC/packing/random |
| 2026-10-10 | A0.4 registry — in-RAM, без NVS | derivation детерминирована → после reboot та же identity, persistence не нужна |
| 2026-10-10 | Entity ensure — только при успешном resolve; ошибка → явная, ложной Entity нет | принцип «не публиковать ложное» |
| 2026-10-10 | `zb_entity_ensure_entity`: rollback binding при падении `domain_entity_put` | атомарность: не оставлять привязку без Entity (идемпотентно переприменится) |
| 2026-10-10 | **A0 завершён**: identity доказана reboot/order/re-interview independent; registry RAM-only; persistent mutable id не нужен | следующий — A1 canonical state (entity_id, property_id) |
| 2026-10-10 | A1 идёт без erase+replace: сперва parity raw↔canonical на одном входе | смена truth — отдельный шаг (A1.4), обратимая |
| 2026-10-10 | A1.2 canonical — тень: пишется только при наличии Entity и маппинга; иначе raw остаётся единственной записью | не выдумывать смысл там, где его нет |
| 2026-10-10 | `ha_state_key_t` содержит явный `reserved` (padding) | Domain сравнивает ключи побайтово; неявный padding делал lookup недетерминированным |

| 2026-10-10 | **Ревизия порядка шага A** (решение): A3→A4→A6 сначала, затем A1.4/A1.5 | читатели адресуют (uid,ep,cluster,attr) и не зависят от `zigbee`; без reverse-map в consumers нельзя честно читать canonical |

**Правило (Domain key structs):** любая key-структура Domain обязана иметь полностью
детерминированное байтовое представление — либо без padding, либо с явными `reserved`,
которые всегда инициализируются нулём (`{0}`). Domain сравнивает ключи побайтово, поэтому
неявный padding делает lookup недетерминированным (проявилось на `ha_state_key_t` в A1.3).

## 5. От чего отказались

```text
сразу полная нормализация (вариант A)   — erase данных + WS v3 ради одного транспорта
property в общем дескрипторе с cluster/attr — Zigbee протёк бы обратно в ha_model
zigbee_property_decode() из потребителей   — связал бы сервисы напрямую
хранение property_id в записях             — превратило бы B в миграцию storage
TLV / самоописание приложения              — запрещено RECORD_MODEL §2
```

## 6. Хронология

```text
2026-10-10  зафиксирован контракт (PROPERTY_MODEL.md) и план миграции; код не менялся
2026-10-10  фаза 0: ha_properties.{h,c} (Property/Action/Event, ha_value_t, ha_property_desc());
            host-тест test_properties 1/1, IDF-сборка зелёная; поведение не менялось
2026-10-10  фаза 1: компонент semantics (мост ZCL↔семантика); приватная zb_property_map_t
            с decode_kind; парный host-тест test_semantics 1/1 (паритет + нормализация + отказы);
            IDF-сборка зелёная; мост ещё не подключён — поведение не менялось
2026-10-10  фаза 2.0–2.2: semantics_property_key + value_to_double; расширен mapping
            (level/hue/sat/xy/color temp + system); Automation декодирует через мост,
            attr_scale и ha_zigbee.h убраны из чистой логики; host test_rule 1/1, IDF зелёная
2026-10-10  фаза 2.3–2.5: Display (ui_widgets: kind/format/apply_color + слайдеры в семантике;
            ui_status_bar: часы через мост) — UI больше не знает ZCL; IAS-вывод (ZONE_STATE ≠
            тревога); IDF зелёная, прошито на P4
2026-10-10  фаза 3.0–3.5: semantic command (ha_command_value_t SCALAR/XY, HA_PROPERTY_COLOR);
            semantics_build_command + zb_action_map; Display → display_send_command (кодирование
            убрано из UI); Automation action остаётся opaque; test_semantics 1/1 (команды),
            IDF зелёная, прошито на P4
2026-10-10  фаза 4.0–4.1: ha_event_t + ha_zb_event_t + ha_event_id_t (общий, вкл. MINUTE_TICK);
            semantics_decode_event (профиль устройства); test_semantics 1/1 (события).
            4.2–4.6 (публикация/System/Automation) — отдельный шаг
2026-10-10  фаза 4.2–4.6: Zigbee публикует semantic EVENT (value=HA_EVENT_*) + shim-payload;
            System → ha_event_id_t (ids 4..7, web-ui SYSTEM_EVENTS правлены); Automation по
            event_id (TIME) + legacy raw через payload; tests 1/1, IDF зелёная, прошито на P4.
            Event plane transport-agnostic
2026-10-10  фаза 5.0 core: semantics_cluster_properties / semantics_property_actions
            (capabilities без inference в UI); test_semantics 1/1. BFF/DTO/фронт — далее
2026-10-10  фаза 5.2: WEB_CMD_SEMANTIC_COMMAND (stable LE DTO) → semantics_build_command;
            web-ui: semantics.js + semanticCommand; EndpointWidgets/StateAttr шлют семантику
            (commands.js без ZCL). Проверено на P4: POWER/BRIGHTNESS/COLOR_TEMP/COLOR доходят,
            toggle переключает состояние. IDF+web-ui зелёные
2026-10-10  фаза 5.0 BFF + 5.1: общий wire-кодек semantic-value (bits_to_value/value_to_bits);
            WEB_MSG_SEMANTIC_STATE/_REMOVE (аддитивно, snapshot+delta, UNKNOWN не шлём); store по
            (uid,ep,property); EndpointWidgets читает семантику. Строгие размеры DTO в 5.2.
            Проверено на P4: 0x12-состояния корректны (system time, POWER, BRIGHTNESS, COLOR, TEMP)
2026-10-10  фаза 5.3.0–5.3.3/5.3.6: ha_sem_rule_t + semantics_compile_automation +
            semantics_event_to_physical; WEB_CMD_SEMANTIC_AUTOMATION_PUT (LE DTO). Host-тест
            компилятора; на P4 проверено (TIME→POWER ON → запись #100). 5.3.4 (UI) — далее
2026-10-10  фаза 5.3.4a: semantics_decompile_automation (physical→semantic) + round-trip и
            отказы в host-тесте. Читающая сторона Automation готова к semantic UI
2026-10-10  фаза 5.3.4b: WEB_MSG_SEMANTIC_AUTOMATION/_REMOVE (общий rule-wire кодек),
            snapshot+delta проекция, browser semAutomations. На P4 приходят 5 правил
            (TIME/STATE/EVENT), representable=1. Пофикшен дедлок (domain-лок) в snapshot
2026-10-10  фаза 5.3.4c/d + snapfix: Automation UI целиком на семантике (automation.js без ZCL,
            semanticAutomationPut, селекторы из states/capabilities, legacy=read-only);
            кэш профилей устройств в snapshot (симметрия snapshot/delta). На P4 проверено:
            PUT STATE temp>25→POWER ON (#200) читается обратно семантически
2026-10-10  фаза 5.4: WEB_MSG_EVENT (0x16), Web сериализует уже HA_EVENT_*; Events UI на
            eventName(), SYSTEM_EVENTS удалён. На P4 приходит system MINUTE_TICK (source_kind=4)
2026-10-10  фаза 5.5.0–5.5.1: semantics_cluster_capabilities; WEB_MSG_SEMANTIC_CAPABILITIES/
            _REMOVE; EndpointWidgets из capabilities. На P4: лампа EP2 → COLOR/COLOR_TEMP/
            BRIGHTNESS/POWER(ON,OFF,TOGGLE); датчик → BATT%,BATT_V,TEMP (ro)
2026-10-10  фаза 5.5.2: semantic group item (facade над raw); WEB_MSG_SEMANTIC_GROUP_ITEM/
            _REMOVE + semantic PUT/REMOVE; Groups.jsx — property+semState. На P4 приходят
            semantic items (lamp ep2: POWER/COLOR_X, ep3: TEMP; system: TIME_UTC)
2026-10-10  фаза 5.5.3–5.5.4: обычные страницы (SystemStatus/DeviceCard/DeviceDetail) только на
            semStates; raw Zigbee вынесен в Diagnostics-экран. grep обычных страниц: ZCL нет
            (кроме CSS-класса). IDF + web-ui зелёные, прошито на P4
2026-10-10  фаза 5.5.5–5.5.7: удалён мёртвый frontend-код (zbCommand/automationPut/
            encodeAutomationRecord/raw groupItem/capabilities.js/commands.js); zcl.js — только
            uidHex/hex16 + Diagnostics. Греп-чек: обычные страницы без ZCL. Прошито на P4
    → **Фаза 5 закрыта**: обычный web-ui не интерпретирует cluster/attr/ZCL;
      ZCL остался только в Diagnostics + transitional Web↔Domain raw ABI (до шага A)
2026-10-10  перед шагом A: baseline зафиксирован (`da30b93`), архитектурный аудит по 8
            направлениям → `STEP_A_PLAN.md` (цель, фазы A0–A8, persistence/failure/identity).
            Открытые решения — §5 плана. Код A не начат
2026-10-10  §5 STEP_A_PLAN закрыт: entity_id + Entity/Binding split, разовый erase,
            minimal WS v3, Diagnostics вне v3, system/weather → entities. Открыт только
            алгоритм derivation entity_id (A0.3). Код A пока не начат
2026-10-10  шаг A, A0.0–A0.1: `ha_model/ha_entity.h` — `ha_entity_id_t` (u64, 0=none),
            `ha_entity_key_t`, минимальный `ha_entity_record_t`; host-тест `test_entity` 1/1.
            Аддитивно, без canonical state/binding/derivation. IDF зелёный
2026-10-10  шаг A, A0.2: `zigbee/zigbee_entity_binding.{h,c}` — `zb_entity_binding_t`
            { entity_id, device_uid, endpoint } (без transport), двусторонний lookup
            physical↔entity; host-тест `test_entity_binding` (zigbee 5/5). §3.1 STEP_A_PLAN
            зафиксировал binding/seed. Аддитивно; IDF зелёный
2026-10-10  шаг A, A0.3: `ha_entity_id_derive` + `ha_siphash24` (fixed key,
            LE32(issuer)||LE32(len)||seed, 0→1); `ha_entity_issuer_t`. Golden + официальный
            SipHash-вектор в `test_entity`. §5 закрыт (algorithm fixed). IDF зелёный
2026-10-10  шаг A, A0.4: `zb_entity_seed`/`zb_entity_resolve` (find→derive→add, идемпотентно,
            order-independent); `zigbee_entity_ensure` (NVS не нужен — derivation детерминирована)
            → Domain `HA_ENTITY_ENTITY` (RAM, зарегистрирован в bootstrap). Bridge ensure-ит Entity
            после интервью. Тесты binding: first/repeat/2-ep/order/reboot/re-pair/endpoint-change/
            capacity (zigbee 5/5). Boot P4 чистый; canonical state пока raw. IDF зелёный
2026-10-10  шаг A, A0.5 — A0 завершён: `zb_entity_ensure_entity` (resolve+ensure, atomic
            rollback при падении Domain) + интеграционный `test_entity_identity` в реальном
            in-memory Domain (first/repeat/order/reboot/re-pair/endpoint-change/atomicity).
            Identity доказана reboot/order/re-interview independent; registry RAM-only намеренно;
            persistent mutable id не нужен. zigbee 6/6, IDF зелёный. Canonical state всё ещё raw
2026-10-10  шаг A, короткий ревью A0 (границы для A1) — зелёный: id только из derive; bridge
            получает id только через binding/resolve; generic Entity API без uid/endpoint;
            ensure — единственная точка создания Zigbee Entity; второго mapping нет; rollback
            симметричен; id не зависит от cluster/attr/order; record минимален.
2026-10-10  шаг A, A1.0–A1.3 — canonical state введён рядом с raw, БЕЗ переключения truth.
            A1.0 форма key/record (ha_state.h). A1.1 таблица HA_ENTITY_PROPERTY_STATE (RAM).
            A1.2 тень: Zigbee report -> entity_id(до binding lookup)+property(semantics) ->
            ha_value_t; raw всё ещё источник, canonical не выдумывается при unmapped/miss.
            A1.3 parity-тест raw-projection == canonical. Найдено: неявный padding в
            ha_state_key_t → byte-lookup в Domain не находил запись; добавлен явный reserved.
            ha_model 3/3, zigbee 7/7, IDF зелёный. A1.4 (переключить readers) — не начат
2026-10-10  шаг A, ревизия порядка: A3→A4→A6 сначала, затем A1.4/A1.5 (см. выше и STEP_A_PLAN).
            Причина: читатели адресуют (uid,ep,cluster,attr) и не зависят от zigbee; без
            reverse-map в consumers нельзя честно читать canonical.
2026-10-10  шаг A, A3.0 — хранимая entity-адресованная форма правила `ha_rule_t`
            (ha_model/ha_rule.h): refs (entity,property), entity-событие, action по entity,
            сентинел HA_ENTITY_ID_SOURCE; детерминированный POD 192B. ha_model 4/4. A3.1+ — дальше
2026-10-10  шаг A, A3.1 — событие entity-тегировано. `ha_event_t` = {id, source_entity, value}
            (source_entity вместо device_uid+endpoint). `semantics_decode_event` источник не
            знает (только event_id) — source_entity ставит adapter через zigbee_entity_lookup.
            Entity-факт EVENT (HA_ENTITY_ENTITY) публикуется параллельно legacy DEVICE-факту
            (payload-shim остаётся для старого engine до A3.3). System-события → ENTITY_SYSTEM
            (ha_entity_singleton(SYSTEM)). ha_model 4/4 (test_entity+singleton), semantics 1/1,
            zigbee 8/8 (test_event_entity: stable source/event_id, endpoint-scoped, system) — IDF зелёный
2026-10-10  шаг A, A3.2 — command-by-entity. HA_CMD_PROPERTY + ha_property_command_t
            (entity, property, action, value). zigbee_entity_physical (reverse lookup) в
            registry. Executor zigbee_property_execute резолвит entity→(uid,ep) и строит
            ha_zb_command_t через semantics_build_command. Потребитель транспорта не видит;
            физическая проверка/enqueue выделена в zigbee_enqueue_command. zigbee 9/9
            (test_command_entity: reverse round-trip, build-command из resolved entity) — IDF зелёный
2026-10-10  шаг A, ревизия порядка №2 (решение): A3.3/A3.4 требуют entity-identity на
            клиентской границе (Automation — одна Domain-таблица с фиксированным payload;
            web читает ту же таблицу, а UI адресует по uid/ep). Без reverse-map в BFF это
            значит: вынести минимальный фундамент A6.0/A6.1 (canonical v3 + frontend identity
            = entity_id), затем вернуться к A3.3–A3.5. Новый порядок:
            A6.0→A6.1→A3.3→A3.4→A3.5→A4→A6.2+→A6.x→A1.4→A1.5.
2026-10-10  шаг A, A6.0 — canonical v3 message set (фонд): WEB_MSG3_ENTITY/PROPERTY_STATE/
            CAPABILITIES (+REMOVE) в web_proto.h и web-ui/proto.js. Обычный v3 без
            uid/endpoint/cluster/attr; identity = entity_id. Живёт рядом с v2 (кадр тот же).
2026-10-10  шаг A, A6.1 (backend, часть) — web отдаёт v3 ENTITY + PROPERTY_STATE из
            canonical (snapshot + delta), отдельными эмиттерами (типы 14/15 вне v2 SCHEMA).
            Только canonical-источники; без uid/endpoint. IDF зелёный. Осталось: CAPABILITIES
            v3 (нужен canonical-источник по entity — сейчас capabilities проецируются из
            ENDPOINT(uid,ep) через мост, а web не имеет binding) и frontend identity = entity_id.
            Найден продуктовый вопрос: ENTITY пока без metadata (имени) → обычный UI не сможет
            показывать имена. Требуется решение (см. диалог): name в ENTITY, owner — adapter.
2026-10-10  шаг A, A6.1a — Entity logical metadata (решение владельца): `ha_entity_record_t`
            = { char name[32] }. Принцип: «adapter создаёт/связывает Entity и может
            инициализировать metadata; далее логической metadata владеет Domain Entity».
            zb_entity_ensure_entity принимает initial_name, при существующей Entity имя НЕ
            перетирает; registry берёт initial name из DEVICE.name. Rename — generic
            HA_CMD_ENTITY_RENAME(entity_id,name) (executor в bootstrap, adapter не участвует),
            web-команда WEB_CMD_ENTITY_RENAME; v3 ENTITY теперь несёт name (40B).
            ha_model 4/4, zigbee 9/9 (test_entity_identity + name init/no-clobber), IDF зелёный
2026-10-10  шаг A, A6.1b — canonical capabilities. HA_ENTITY_CAPABILITIES (ключ entity_id;
            форма ha_capabilities.h, record 196B). Мост: semantics_endpoint_capabilities
            (server-кластеры endpoint → properties/actions, кластеры не сохраняются). Adapter
            пишет при интервью (после ensure Entity). web отдаёт v3 CAPABILITIES (snapshot+delta).
            Все три canonical read-сущности v3 готовы (ENTITY/PROPERTY_STATE/CAPABILITIES);
            источника uid/ep нет. semantics 1/1, ha_model 4/4, zigbee 9/9, IDF зелёный.
            Осталось: frontend identity = entity_id (A6.1c).
2026-10-10  шаг A, A6.1c — frontend identity = entity_id (A6.1 завершён). store: canonical
            v3 maps semEntities/propStates/propCapabilities; semState(entity,property),
            capabilities(entity), entityName, entityProperties; v2 semState → semStateLegacy.
            Обычный UI: Entities/EntityDetail/EntityCard/EntityWidgets на entity; rename →
            entityRename → WEB_CMD_ENTITY_RENAME → HA_CMD_ENTITY_RENAME; control →
            propertyCommand → WEB_CMD_PROPERTY_COMMAND → HA_CMD_PROPERTY (A3.2). Удалены
            DeviceCard/DeviceDetail/EndpointWidgets/Devices и endpointCapabilities/renameDevice;
            device-remove перенесён в Diagnostics. uid/ep остаются только в Diagnostics (raw),
            Groups/Automation/SystemStatus (transitional A4/A6.2). web-ui + IDF зелёные.
            Дальше: возврат к A3.3 (engine на ha_rule_t) → A3.4 → A3.5.
2026-10-10  шаг A, A3.3 (ядро) — чистый semantic-evaluator `automation_rule_sem`
            (automation_rule_sem.{h,c}): resolve(SOURCE→src), event_matches по (source,event_id),
            time_matches, state_matches по (entity,property) с edge/BETWEEN, compare,
            condition_ok, action → ha_property_command_t. Только entity/property/action — без
            uid/endpoint/cluster/attr. Host test test_rule_sem (7 сценариев). automation 2/2,
            IDF зелёный. Подключение к Domain (engine читает HA_ENTITY_AUTOMATION=ha_rule_t,
            PROPERTY_STATE, HA_CMD_PROPERTY) неотделимо от A3.4: таблица automation должна
            стать ha_rule_t, а web читает/пишет ту же таблицу → нужен Automation v3 wire (A6.2).
2026-10-10  шаг A, A3 блок (шаг 0) — canonical System properties (prereq для TIME).
            system.c put_state параллельно пишет HA_ENTITY_PROPERTY_STATE (ENTITY_SYSTEM,
            property) через мост — Automation/TIME читают время как семантику, не cluster/attr.
            system REQUIRES semantics. IDF зелёный. Далее: A6.2 Automation v3 wire → A3.4
            storage cutover (ha_rule_t) → A3.3 wiring engine → Web/UI cutover → удаление
            compile/decompile physical automation → A3.5 behaviour parity.
2026-10-10  шаг A, A3 block целиком (A6.2 Automation + A3.4 storage + A3.3 wiring + удаление):
            • HA_ENTITY_AUTOMATION payload = ha_rule_t (persist_key automation_v2 → erase).
            • engine (automation.c) на entity: EVENT(HA_ENTITY_ENTITY), STATE/conditions
              (PROPERTY_STATE), TIME (canonical SYSTEM), action HA_CMD_PROPERTY. uid/ep/cluster/
              attr/command_id engine не знает. SOURCE → source триггера (TIME → ENTITY_SYSTEM).
            • web v3 AUTOMATION (0x36/0x37) + WEB_CMD_AUTOMATION_PUT(entity rule) — без compile.
            • UI AutomationForm/Automations entity-native (entity_id/property/event/action).
            • Удалено: automation_rule.{c,h}, ha_automation_record_t/condition, ha_sem_rule_t,
              semantics_compile/decompile_automation, semantics_event_to_physical, v2 semantic
              automation message/command, physical rule codec frontend.
            • Behavior parity test (TIME→ON, TEMP>25→ON, EVENT→TOGGLE, cond+SET, SOURCE).
            automation 1/1, semantics 1/1, ha_model 4/4, zigbee 9/9, IDF + web-ui зелёные.
            A3 реально закончена. Дальше: A4 (group_item semantic), затем A6.2+ остаток, A1.4/A1.5.
2026-10-10  шаг A, ревью A3-цепочки — границы EVENT→ha_rule_t→engine→HA_CMD_PROPERTY→adapter:
            uid/endpoint/cluster/attr в rule-model/engine/wire отсутствуют (только в комментариях);
            storage — только ha_rule_t (без physical shadow); transitional (compile/decompile,
            ha_automation_record_t, ha_sem_rule_t, SEMANTIC_AUTOMATION) остались только в
            комментариях. Failure-семантика: нет entity/property/значения → условие false или
            триггер пропускается; unmapped action/нерезолвимая цель → команда не постится (лог).
            Найдено/исправлено: правило с HA_ENTITY_ID_SOURCE могло совпасть при source_entity==NONE
            (resolve давал NONE==NONE). Добавлены guard'ы (source/entity NONE → no match) +
            тесты. automation 1/1, IDF зелёный.
2026-10-10  шаг A, A4 — group_item entity-native. ha_group_item_key_t.state = ha_state_key_t
            (entity, property); persist_key group_item_v2 (erase старого physical ABI). Web:
            семантический group_item wire стал canonical (group_id, entity, property, order,
            title) — мост не нужен; raw group_item path и WEB_CMD_GROUP_ITEM_* удалены. UI
            Groups — entity-native (entity+property из capabilities). Display: виджет читает
            canonical PROPERTY_STATE для значения и соседних свойств (X/Y/яркость) той же
            Entity; команды → HA_CMD_PROPERTY; заголовок виджета — имя Entity. physical
            ha_zb_state_key_t из Display ушёл. ha_model 4/4, semantics 1/1, zigbee 9/9,
            automation 1/1, IDF + web-ui зелёные. Дальше: A6.2+ (остальные v3/удаление v2),
            A1.4 (остальные readers), A1.5 (raw cleanup).
2026-10-10  шаг A, A6.2+ — canonical v3 complete + v2 semantic cleanup. EVENT стал v3
            (WEB_MSG3_EVENT: u64 source_entity + u8 event_id + value; без source_uid/endpoint/raw).
            Удалены v2 semantic сообщения/каналы: SEMANTIC_STATE(+REMOVE), SEMANTIC_CAPABILITIES
            (+REMOVE), EVENT, SEMANTIC_AUTOMATION(+REMOVE), физический group-item wire и
            web_do_group_item_*; web.c больше не проецирует raw STATE/ENDPOINT в semantic.
            SystemStatus (frontend) и ui_status_bar (Display) читают canonical
            PROPERTY_STATE(ENTITY_SYSTEM) — ENTITY_SYSTEM=0x9229b47704a245b5 (golden сверен).
            Обычный frontend зависит только от canonical v3 (ENTITY/PROPERTY_STATE/CAPABILITIES/
            AUTOMATION/GROUP_ITEM/EVENT) + commands (PROPERTY/ENTITY_RENAME/AUTOMATION/GROUP_ITEM).
            Остаётся raw ENTITY-состояние как отдельный диагностический канал (снимется в A1.5).
            ha_model 4/4, semantics 1/1, zigbee 9/9, automation 1/1, IDF + web-ui зелёные.
2026-10-10  шаг A, A6 review + A1.4/A1.5. Ревью A6 зелёный: обычный контракт entity-native,
            без uid/endpoint/cluster/attr, read/write симметричны, fallback к raw нет.
            A1.5 (выбран вариант 1 с границей имени): raw state вынесен в diagnostics-only —
            HA_ENTITY_STATE → HA_ENTITY_ZB_DIAG_STATE; отдельный канал WEB_MSG_DIAG_ZB_DEVICE/
            ENDPOINT/STATE/REMOVE (вне web_msg3_t, transport-specific); Diagnostics.jsx читает
            diag-канал и стал read-only (удалён SEMANTIC_COMMAND); raw ENTITY DEVICE/ENDPOINT/
            STATE убраны из обычного snapshot; удалены неиспользуемые WEB_CMD_ZB_COMMAND/
            DEVICE_RENAME/SEMANTIC_COMMAND и их обработчики. PROPERTY_STATE — единственный
            production state; grep HA_ENTITY_STATE = 0. ha_model 4/4, semantics 1/1, zigbee 9/9,
            automation 1/1, IDF + web-ui зелёные. Осталось по плану: A7 (erase старого physical
            state ABI), A8 (удаление Dial/weather/location/wifi из raw-классификации при желании).
2026-10-10  шаг A, A7 — разовый erase старого persistent ABI. app_main::erase_legacy_mstore_once
            (marker в NVS `ha_boot/stepA_erase`) стирает раздел `mstore` один раз до domain_init;
            старые physical state/automation/group_item после cold boot не подхватываются, откат
            не поддерживается. Добавлен docs/STEP_A_FINAL_STATE.md (инварианты, слои, WS v3,
            diagnostics, не-Entity подсистемы, migration/rollback). IDF зелёный.
2026-10-10  шаг A, A8 cleanup (только удаление мёртвого, без изменения модели). Удалено:
            legacy DEVICE-keyed event publish + physical ha_zb_event_t payload (событие теперь
            только entity-адресованное); semantics_property_key / cluster_properties /
            property_actions + map_find_by_property (+ тесты); web semantic_automation_payload,
            ha_zb_command_t static_assert; мёртвые экспорты zcl.js. Переименовано
            SEMANTIC_GROUP_ITEM→GROUP_ITEM, web_send_semantic_event→web_send_event; поправлены
            устаревшие комментарии. Weather/Location/WiFi оставлены отдельными config/status
            подсистемами. grep: удалённые символы = 0. ha_model 4/4, semantics 1/1, zigbee 9/9,
            automation 1/1, IDF + web-ui зелёные. Шаг A полностью завершён.
2026-10-10  пост-Step-A: Automation Runtime Index (производный RAM-индекс триггеров).
            automation.c держит компактный массив ключей {rule_id, kind, entity, property,
            event_id}; на старте и на любое upsert/remove правила (подписка на
            HA_ENTITY_AUTOMATION, любой источник) — полная перестройка. На EVENT/STATE/TIME
            ищем кандидатов по индексу (SOURCE матчит любой адрес), затем читаем только
            подходящие правила и evaluate. Source of truth — только HA_ENTITY_AUTOMATION;
            индекс не persistится, восстанавливается из правил; mstore/Domain не менялись.
            ha_model 4/4, semantics 1/1, zigbee 9/9, automation 1/1, IDF зелёный.
2026-10-11  шаг A+: детерминированное initial-имя Entity в Zigbee adapter. Одна Entity у
            устройства → имя DEVICE; несколько → "<device> - <endpoint>" (endpoint, не порядок
            discovery). Задаётся только если Entity.name пусто (создание или запись без имени);
            после rename не перетирается. Правило вынесено в host-testable
            zb_entity_initial_name (zigbee_entity_binding) + тест. zigbee 9/9, IDF зелёный.
2026-10-11  Step B, B1 — пассивная модель Logical Device. ha_model/ha_logical_device.h:
            ha_device_id_t (uint64), HA_DEVICE_ID_NONE, ha_logical_device_key_t (id),
            ha_logical_device_record_t {name[32]}. Новый тип HA_ENTITY_LOGICAL_DEVICE=17
            (ha_entities.h). Поля device_id в ha_entity_record_t НЕ добавлено (это B2).
            host test test_logical_device (sizes/POD-layout/zero-bytes/none/type-distinct).
            Zigbee-зависимостей в ha_model нет. ha_model 5/5, IDF зелёный.
2026-10-11  Step B, B2 — ownership-связь Device → Entity. ha_entity_record_t += device_id
            (8 + 32 = 40, offsetof(name)=8); HA_ENTITY_ENTITY — RAM-таблица, поэтому смена ABI
            без flash-erase (восстанавливается на интервью). Adapter: zb_device_id(uid) =
            derive(ZIGBEE, LE(EUI64), 8); registry передаёт device_id в zb_entity_ensure_entity
            (guard device_id != NONE). Ownership host-тест: один device → общий device_id на
            всех endpoint; другой device → свой; повторный ensure/reboot не меняют device_id;
            смена endpoint меняет entity_id, не device_id. Domain/ha_model EUI64 не знают.
            ha_model 5/5, semantics 1/1, automation 1/1, zigbee 9/9, IDF зелёный.
2026-10-11  Step B, B3 — adapter-private device binding (device_id ↔ EUI64). Отдельный модуль
            zigbee_device_binding.{h,c}: find_device_id/find_uid/add/remove. Инварианты:
            device_id != NONE; биекция uid↔device_id; идемпотентный add; конфликт/NONE/нет
            места → false; RAM-only. Не смешан с entity binding (entity_id ↔ EUI64+endpoint).
            host test test_device_binding. Нужен для B7 (device_id → EUI64 → unpair). zigbee
            10/10, IDF зелёный. Дальше B4: interview создаёт Logical Device + связывает Entity.
2026-10-11  Step B — B3 ОТКАЧЕН (решение): отдельный device-binding registry `device_id ↔
            EUI64` признан лишней инфраструктурой. Forward `device_id = derive(ZIGBEE, EUI64)`
            вычисляется всегда, reverse нужен лишь для редкого unpair → делается scan'ом
            известных Zigbee-устройств, а не вторым registry. Entity binding остаётся (горячий
            путь команды). Удалены zigbee_device_binding.{h,c} + тест; CMake восстановлен;
            STEP_B_PLAN обновлён (B3 снят, B4/B7 уточнены). Принцип: не строить общий индекс,
            пока дешёвый scan закрывает редкий сценарий.
2026-10-11  Step B — пересмотр модели + B1'. Принцип Step B: «adapter owns physical
            representation, Device service owns normalization into canonical Device graph,
            Domain only stores that graph». Logical Device record расширен provenance:
            {parent_device_id, source, native_id[32], name[32]} (80B, без неявного padding);
            добавлен ha_device_desc_t (DTO от adapter: source, native_id, parent_native_id,
            name) и единая generic derivation ha_device_id_derive(source, native_id) в ha_model
            (без альтернативных формул в adapter). zb_device_id теперь канонизирует EUI64 в
            lowercase hex native_id и зовёт ha_device_id_derive. Незакоммиченный наивный B4
            (adapter сам писал Domain) откачен. STEP_B_PLAN перезаписан под source/native_id/
            parent, boundary Device service, порядок B1'..B9. ha_model 5/5, zigbee 9/9, IDF зелёный.
2026-10-11  Step B, B3' — generic Device service (компонент `device`, над Domain, без
            транспорта). device_register(domain, origin, ha_device_desc_t, out_id): derive
            device_id/parent_device_id, upsert Logical Device (имя не перетираем). device_bind_
            entity: ownership Entity.device_id (Entity должна существовать → NOT_FOUND).
            device_remove: снять ownership (iter+filter по device_id, не мутируя внутри iter) +
            удалить Logical Device. Adapter-specific unpair — через dispatcher выше (B7), не
            отсюда. Граница: UI/бизнес → Device service → Domain; Domain adapter не вызывает.
            Добавлен SYS_LAYER_DEVICE. Корневой CMake: EXTRA_COMPONENT_DIRS += device.
            host test test_device_service (register/idempotent/parent/bind/remove). IDF зелёный.
            Дальше B4': Zigbee интервью строит нормализованный Device graph и зовёт Device service.
2026-10-11  Step B, B4' — Zigbee подаёт нормализованный Device graph в Device service.
            Ownership-запись убрана из adapter: zb_entity_ensure_entity больше НЕ пишет
            Entity.device_id (только identity/имя); device_id-параметр удалён. zigbee.c при
            interview: zb_device_native_id(uid)→desc{source=ZIGBEE, native_id, name=DEVICE.name}
            → device_register(root) → device_id; для каждого endpoint ensure Entity →
            device_bind_entity(device_id, entity_id) + capabilities. app_main регистрирует
            HA_ENTITY_LOGICAL_DEVICE (RAM), APP_ENTITY_TYPES 17; zigbee REQUIRES device.
            device_bind_entity отклоняет reassign (уже привязана к другому Device → INVALID_STATE).
            Тесты: device 1/1 (+reassign), zigbee 9/9 (ownership убран из adapter; ensure не
            выставляет device_id), IDF зелёный.
2026-10-11  Step B, B4' hardware smoke — PASS. На P4 реальное интервью устройства
            (ESP32C6-DISPLAY, uid=9888e0fffe6c1554, 4 endpoint): Logical Device создан один раз
            (id=71b213ac527a6357, native=9888e0fffe6c1554, source=ZIGBEE); каждый endpoint →
            Entity с device_id == Logical Device (bind ok=1). device_id сверен с hand-computed
            derivation (MATCH). После reboot device_id тот же (детерминирован), ownership
            восстановлен (device_register идемпотентен); второе устройство (uid=20a716…,
            1 endpoint) → свой id.             Добавлены info-логи zigbee: device:/entity->device.
2026-10-11  Step B, B5 — WS v3 + UI: Device → Entity grouping. Backend: WEB_MSG3_DEVICE/
            _REMOVE (device_id, parent_device_id, name) из HA_ENTITY_LOGICAL_DEVICE (snapshot+
            delta); ENTITY v3 DTO расширен device_id (48B). Frontend: store semDevices +
            semEntities.deviceId; Entities группирует по Device (дерево по parent; «Без
            устройства» отдельно). Проверено на P4 скриптом: DEVICE(parent=0) и ENTITY с
            device_id приходят; первый ENTITY-кадр при create (device=NONE), затем на bind
            (owner) — ожидаемая двухшаговая ensure→bind граница. IDF + web-ui зелёные.
2026-10-11  Step B, B6 — rename Logical Device. device_rename(domain, origin, device_id, name)
            меняет только name (native_id/source/parent и Entity.name не трогает); отсутствующий
            Device → NOT_FOUND. device_register: initial name из adapter заполняет только пустое
            имя (существующее пользовательское не перетирается). Web: WEB_CMD_DEVICE_RENAME
            (u64 device_id + name[32]) → device_rename (UI origin); web REQUIRES device. UI:
            редактируемое имя Device в группе. Проверено на P4: device 71b213ac527a6357
            "" → "Кухня" (наблюдалось в snapshot). device 1/1, IDF + web-ui зелёные.
2026-10-11  Step B, B7 — removal lifecycle + adapter dispatcher. Device service: registry
            remover'ов по source (device_register_remover); device_remove(device_id) читает
            Device {source, native_id} → remover(source) (Zigbee: zb_uid_from_native_id → пометка
            DEVICE_REMOVE + leave best-effort, без ожидания физ. устройства) → при успехе cascade:
            Entity + PROPERTY_STATE + CAPABILITIES + GROUP_ITEM refs удаляются, automation refs
            СОХРАНЯЮТСЯ (broken/unresolved), adapter entity bindings забываются
            (zigbee_entity_forget), Logical Device удаляется. Ошибка remover → canonical не
            трогаем. UI: LOGICAL_DEVICE_REMOVE(device_id) + кнопка «Удалить». host test device
            (cascade + remover + повторное удаление → NOT_FOUND). device 1/1, IDF + web-ui зелёные.
            (Hardware-removal smoke не триггерился — разспарит реальное устройство.)
2026-10-11  Step B, B8 — rename старого uid-keyed transport device. HA_ENTITY_DEVICE →
            HA_ENTITY_ZB_DIAG_DEVICE, WEB_ENTITY_DEVICE → WEB_ENTITY_ZB_DIAG_DEVICE, frontend
            ENTITY.DEVICE → ENTITY.ZB_DIAG_DEVICE (+ key в schema.js); DEVICE_REMOVE-варианты не
            тронуты. Raw Zigbee device (EUI64-ключ, name/model) — adapter-private и только
            diagnostics (канал WEB_MSG_DIAG_ZB_DEVICE); ordinary Web/UI на него не подписан.
            Логическая модель Device не менялась. grep старых имён = 0. ha_model 5/5,
            semantics 1/1, automation 1/1, device 1/1, zigbee 9/9, IDF + web-ui зелёные.
2026-10-11  UI: карточка-устройство (Logical Device) возвращена — DeviceCard с именем
            устройства и Entity-контролами/значениями внутри (дерево по parent). Удаление
            устройства перенесено с Diagnostics (raw uid, v2) на страницу «Устройства»
            (LOGICAL_DEVICE_REMOVE); Diagnostics — read-only. Удалены dead proto device-remove
            (v2 uid) и EntityCard. Initial name Logical Device = DEVICE.name или DEVICE.model
            (напр. ESP32C6-DISPLAY/SNZB-01P). Заодно подтверждён re-pair: тот же device_id
            (ad8745aca6c843cb) и entity_id (274002a78a9cd8bb) после rejoin. Зафиксирован
            docs/STEP_C_PLAN.md (план Step C; diagnostics отложены). IDF + web-ui зелёные.
2026-10-11  UI: списки Entity (AutomationForm, Groups WidgetForm) больше не показывают голые
            id — Entity сгруппированы по устройству (optgroup: имя устройства), внутри — имя
            Entity либо короткий id + подсказка по capability. Понятно, что за что отвечает,
            даже когда у Entity своего имени нет.
2026-10-11  Step F (часть 1) — Boundary Purge: удалён мёртвый legacy-путь удаления
            HA_CMD_DEVICE_REMOVE (+ zigbee_remove_execute; zigbee_request_remove/remover
            остаются). Automation больше не фильтрует по DOMAIN_SOURCE_ZIGBEE: подписка по
            canonical типу факта (source_mask 0). semantics_value_to_double перенесён в
            ha_model как ha_value_to_double; automation/display перестали зависеть от semantics,
            у web убрана мёртвая зависимость. IDF + host-тесты зелёные.
2026-10-11  Zigbee/semantics audit — найден и убран мёртвый код/костыли: zb_device_id
            (derivation перенесена в device_register), zigbee_device_ensure (device создаёт
            zigbee_topology_apply), zigbee_diag_snapshot+counters (диагностика удалена в C),
            и мёртвая обвязка payload события (zigbee_event_t.payload / ha_zb_event_t.payload —
            semantics_decode_event их не читает). Adapter пишет только канонические сущности.
2026-10-11  Step E, E5 — убран мёртвый zigbee_value_compact (оставался от удалённого
            zigbee_state_apply); граница value-типов зафиксирована комментарием: Domain
            domain_value_t (журнал) и canonical ha_value_t — один vocabulary (совпадающие
            числа), объявлены отдельно из-за границы слоёв.
2026-10-11  Step E (E1–E4) — все клиентские/транспортные абстракции переведены на device-domain.
            E1: событие — канонический факт (payload = ha_event_t), убрано дублирование
            event_id (meta.value + u8 payload); web/automation читают payload. E2: EVENT DTO
            несёт канонический value (kind+bits). E3: ретайр v2-канала — LOCATION/WEATHER/
            WIFI_STATUS/SETTINGS/GROUP идут новыми entity-native v3-сообщениями (0x3B..0x44);
            удалены WEB_ENTITY_*, WEB_MSG_ENTITY(_REMOVE), web.c SCHEMA/schema_for, web-ui
            schema.js. E4: журнал «События» — одна typed-модель (kind + канонические поля),
            без entityType/action/record. IDF + web-ui собираются, все host-тесты зелёные,
            wire подтверждён (LOCATION/WEATHER/WIFI/GROUP).
2026-10-11  Fix — журнал показывал «объект типа undefined» для изменений состояния.
            Причина: pushEvent-объект имел два ключа `kind` ({kind:'property',…,kind,value}),
            числовой value_kind перезатирал строку 'property', describe не распознавал
            запись. Переименовано в valueKind. Плюс system-источник в логе — «Система».
2026-10-11  UI — журнал «События»: general-сущности (location/group/weather/wifi/settings/
            automation) больше не «сущность обновлена», а «изменено: экран «Кухня» #2» —
            тип + title/name из записи или #id из ключа; неизвестный тип показывает номер.
2026-10-11  UI — журнал «События» не показывал изменения состояния: Events наполнялся
            только из MSG3.EVENT (кнопки/тики) и legacy v2 MSG.ENTITY. Канонические
            PROPERTY_STATE-дельты не логировались. Добавлено логирование изменений
            PROPERTY_STATE (только реальные изменения, не во время snapshot, system
            исключён — иначе минутные тики забивают лог), формат «Entity: свойство = значение».
2026-10-11  UI — AutomationForm: список Entity фильтруется по типу триггера. «Событие»
            показывает только Entity с событиями (capabilities.events), «Состояние»,
            условия и действие — только Entity со свойствами/действиями. При смене типа
            выбранная Entity снапится к первой подходящей. Собирается, прошито.
2026-10-11  Fix — автоматизации не сохранялись. Причина: off-by-one в web_rule_decode:
            предпроверка action-хвоста была NEED(13), а минимум (entity8+property2+
            action1+value_kind1) = 12. Действия ON/OFF/TOGGLE (value_kind=NONE) давали
            ровно 12 байт и правило 26 байт отклонялось как INVALID_SIZE (лог
            "command 4 rejected: code=7"); SET/XY (17/20) проходили. Исправлено на NEED(12).
            Проверено вживую: TIME-правило с action POWER/ON сохраняется и возвращается
            (дельта + snapshot).
2026-10-11  Step D, D1..D4 — полный semantic interface Entity. Canonical capabilities теперь
            две независимые части: properties[] (property+actions) и events[] (press-события).
            semantics.endpoint_capabilities сам собирает обе: server-кластеры → properties,
            client-кластеры → events (cluster_events — внутренняя деталь, наружу не торчит).
            WS CAPABILITIES DTO расширен (u8 property_count, props[], u8 event_count, events[]).
            UI: Actions/Properties и Events берутся из capabilities Entity (fallback к словарю
            только пока caps неизвестны); словари остаются лишь как labels. Smoke: ep1
            ESP32C6-DISPLAY (client OnOff) → events=[SINGLE]; лампа ep2 → events=[].
            Словарь ZB-кластеров не расширялся (отдельный шаг).
2026-10-11  Step C, C3+C4+C5+C7 (один связный коммит) — граница Zigbee замкнута.
            Диагностика удалена целиком (ZB_DIAG_DEVICE/STATE, ENDPOINT, WEB_MSG_DIAG_ZB_*,
            страница Diagnostics, zcl.js, store-map'ы). Raw адаптера (имя/модель +
            топология) переведён в Zigbee-private RAM-store (zigbee_device_store). ZCL-словарь
            и физические формы уехали из ha_model в мост semantics (ha_zigbee.h,
            zigbee_forms.h, zb_command.h). System публикует canonical сам (ZCL его больше не
            касается). Введён нейтральный ha_device_id_t для generic-ключей. Итог: вне
            zigbee/+semantics/ generic core не знает EUI64/endpoint/cluster/attr/zcl. IDF +
            все host-тесты (ha5 sem1 auto1 device1 zb9 domain9) зелёные; web-ui и IDF собираются.
2026-10-11  Step C, C6 — deferred-leave стал Zigbee-private (zigbee_remove_mark, NVS-blob).
            Убраны HA_ENTITY_DEVICE_REMOVE и ha_device_remove_record_t из generic Domain/
            ha_model (web_proto WEB_ENTITY_DEVICE_REMOVE, app_main device_remove_desc).
            zigbee.c/zigbee_radio.c используют приватный стор. IDF зелёная, zb/domain тесты
            зелёные. (C5 выяснил: ha_device_uid_t кросс-разрезный — wifi/display/web/system.)
2026-10-11  Step C, C2 — raw ZCL-команда вынесена из ha_model в мост semantics
            (semantics/zb_command.h: ha_zb_command_t, HA_ZB_COMMAND_ARGS_MAX). Удалён
            HA_CMD_ZIGBEE_CLUSTER(1) и его executor в zigbee (producer'а нет). Domain
            больше не знает о ZCL. IDF зелёная, ha/sem/auto/device/zb/domain тесты зелёные.
2026-10-11  Step C, C1 — убран старый Web Zigbee-control/remove path. Удалены
            WEB_CMD_DEVICE_REMOVE(6)/DEVICE_REMOVE_CANCEL(7) + их обработчики (raw uid → HA_CMD
            ZIGBEE/DEVICE_REMOVE); удаление — только canonical WEB_CMD_LOGICAL_DEVICE_REMOVE.
            Diagnostics — read-only. IDF зелёный.
```

## 7. Открытые вопросы

1. ~~Имя и расположение модуля-моста~~ — решено: `semantics` (`REQUIRES ha_model`); публичный API — семантический.
2. Нужно ли двигать `ha_zigbee.h` из `ha_model` в мост, и кто ещё его импортирует.
3. Дробить ли фазу 5 на под-чекпоинты (по страницам фронта).
4. Нужен ли аддитивный тег «источник транспорта» в событии/состоянии (сейчас не нужен).
5. Как быть с устройствами, где один endpoint = несколько логических сущностей (влияет на
   будущую A, не на B).
6. `ha_value_t` vs `domain_value_t` — сводить ли к одному типу (форма близка).
7. Форма профиля события (по модели устройства / по cluster+command) и где он живёт.
