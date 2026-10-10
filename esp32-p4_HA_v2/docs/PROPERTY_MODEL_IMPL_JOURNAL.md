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
