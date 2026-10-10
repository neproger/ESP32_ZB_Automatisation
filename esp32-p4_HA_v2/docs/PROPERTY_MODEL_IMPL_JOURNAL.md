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
