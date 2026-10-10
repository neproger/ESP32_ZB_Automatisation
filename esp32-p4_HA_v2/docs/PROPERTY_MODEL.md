# Семантическая модель: Property / Action / Event

> **Статус:** принятое решение — **вариант B** (семантический слой без смены
> canonical identity). Карта системы — `ARCHITECTURE.md`. Единая модель записей —
> `RECORD_MODEL.md`. План и чекпоинты миграции — `PROPERTY_MODEL_IMPL_JOURNAL.md`.
>
> Этот документ заменяет первоначальную идею «сразу полностью нормализовать всё»
> (вариант A). A не отменён, а отложен: см. §6–§7.

## 1. Роль

Верхние слои (Automation, Display, Web, Agent) должны работать с **семантикой дома**, а
не с координатами Zigbee. Сегодня они сами толкуют `cluster_id`/`attr_id`. Цель — ввести
общий транспорт-агностичный словарь **Property / Action / Event**, а перевод
ZCL ↔ семантика оставить на границе Zigbee.

**Это не новая сущность и не смена хранения.** Canonical identity состояния и геометрия
таблиц остаются прежними. Меняется только то, *кто и как* придаёт состоянию смысл.

## 2. Проблема (что именно протекло)

Domain чист: типы сущностей и команды — opaque, ключи и записи — байты по дескриптору
(`domain/CMakeLists.txt` не линкует `ha_model`; маршрутизация команд —
`число → callback`, `domain/src/domain_command.c:48-91`).

Протечка выше Domain:

```text
ha_model/ha_zigbee.h          словарь Zigbee (cluster/attr/type/command) лежит в ОБЩЕМ ha_model,
                              который линкуют все сервисы
ha_entities.h:74-92           canonical state key = ZCL-координата (device_uid, endpoint, cluster, attr)
ha_commands.h:53-60           команда = (cluster_id, command_id)
```

и каждое из этих мест **независимо** знает, что `0x0402/0x0000 = температура`:

```text
automation_rule.c:39-51   attr_scale()
display/ui_widgets.c:33-89 ui_widget_kind_for(), format_value()
web-ui/src/zcl.js:87-101   describeAttr()
web-ui/src/capabilities.js:12-31
web-ui/src/commands.js, automation.js
```

Правило проекта «одно решение — один ответственный» (`RECORD_MODEL.md`, AGENTS §3.1)
нарушено: смысл одной пары `(cluster, attr)` размазан минимум по четырём реализациям.

## 3. Решение (вариант B)

```text
physical identity (не меняем):   (device_uid, endpoint, cluster_id, attr_id)
        ↓ property_resolve(...)
semantic property:               HA_PROPERTY_TEMPERATURE / POWER / BRIGHTNESS / ...
        ↓
Automation / Display / Web / Agent — семантика; physical ref — opaque address
```

Ключевой тезис: **ZCL остаётся identity физического состояния, но перестаёт быть его
семантикой для верхних слоёв.**

**Инвариант B.** Потребитель вправе переносить physical reference
(`device_uid/endpoint/cluster/attr`) как **непрозрачный адрес** — хранить, копировать,
передавать — но **не вправе интерпретировать** `cluster/attr/command` и ветвиться по ним.
До A Zigbee-identity физически ещё присутствует в записях, но Zigbee-**смысл** уже не
протекает: им владеет только мост.

Обобщаем не только state, но и команды/события — иначе половина протечки останется:

```text
PROPERTY   POWER, BRIGHTNESS, COLOR_TEMP, TEMPERATURE, HUMIDITY, BATTERY_PERCENT, ...
ACTION     ON, OFF, TOGGLE, SET
EVENT      SINGLE_PRESS, DOUBLE_PRESS, HOLD, ...
```

## 4. Два словаря (не путать)

### 4.1. Общий, транспорт-агностичный (`ha_model`)

Только смысл, без Zigbee-кодировки:

```c
typedef enum {
    HA_PROPERTY_UNKNOWN = 0,
    HA_PROPERTY_POWER,
    HA_PROPERTY_BRIGHTNESS,
    HA_PROPERTY_COLOR_TEMP,
    HA_PROPERTY_COLOR,        /* оттенок/цвет */
    HA_PROPERTY_TEMPERATURE,
    HA_PROPERTY_HUMIDITY,
    HA_PROPERTY_ILLUMINANCE,
    HA_PROPERTY_OCCUPANCY,
    HA_PROPERTY_BATTERY_PERCENT,
    HA_PROPERTY_BATTERY_VOLTAGE,
    /* system-девайс: */
    HA_PROPERTY_TIME_HOUR, HA_PROPERTY_TIME_MINUTE, HA_PROPERTY_WEEKDAY, ...
} ha_property_id_t;

typedef struct {
    ha_property_id_t id;
    ha_value_kind_t  value_kind;   /* number / bool / enum */
    ha_unit_t        unit;         /* celsius / percent / kelvin / none ... */
    float            range_min;
    float            range_max;
    uint8_t          flags;        /* общие подсказки UI (напр. read-only) */
} ha_property_desc_t;
```

**Чего здесь нет:** `cluster_id`, `attr_id`, `zcl_type`, `signed`, `scale`, `offset`.
Размер «×0.01» — это особенность ZCL-кодировки, а не свойство температуры: у Matter та же
`HA_PROPERTY_TEMPERATURE` будет закодирована иначе.

### 4.2. Нормализация ZCL ↔ семантика (грань Zigbee)

Единственный владелец перевода. Живёт в отдельном модуле-мостике (см. §5), а не в
`ha_model` (иначе Zigbee протечёт обратно) и не размазан по потребителям:

```c
typedef enum {
    ZB_DECODE_IDENTITY = 0,     /* raw как есть */
    ZB_DECODE_SCALE,            /* raw * scale + offset */
    ZB_DECODE_ILLUMINANCE,      /* ZCL measured value → lux */
    ZB_DECODE_MIREDS_TO_KELVIN, /* 1e6 / mired */
    ZB_DECODE_BITMAP_BOOL,      /* bitmap → bool */
} zb_decode_kind_t;

typedef struct {
    uint16_t         cluster_id;
    uint16_t         attr_id;
    ha_property_id_t property;
    uint8_t          expected_type; /* optional: validation/diagnostics */
    uint8_t          decode_kind;   /* zb_decode_kind_t */
    float            scale;         /* только для ZB_DECODE_SCALE */
    float            offset;
} zb_property_map_t;
```

Числовое декодирование идёт по **фактическому** `state->zcl_type` (он уже в
`ha_zb_state_record_t`): знак и ширину задаёт ZCL-тип, а не таблица. `expected_type` — лишь
для валидации/диагностики: устройство вправе прислать другой допустимый тип, и он не должен
быть прочитан ложно. Отдельный `signedness` не нужен.

`scale`/`offset` — лишь **частный случай** (`ZB_DECODE_SCALE`): транспортное представление не
всегда линейно (illuminance, mired→Kelvin, bitmap→bool). Поэтому стратегия декодирования
задана **явно** (`decode_kind`), а не зашита как `raw*scale+offset`. Это `enum`, а не
callback: специализированных преобразований немного, они предсказуемы и дешевле для embedded.

Плюс таблицы для действий и событий, но с оговорками:

```text
действие:  (property, action)  → (cluster_id, command_id, args encoder)
           target = физический (device_uid, endpoint) — задаёт ВЫЗЫВАЮЩИЙ, а не mapping
событие:   маппер допускает профиль (device/model) + payload-декодер;
           одной статической таблицей (cluster/command → SINGLE_PRESS) не ограничиваемся —
           разные производители кодируют press/hold/double разными командами, payload и
           vendor-кластерами
```

Физический `device_uid + endpoint` в действии **не** выводится из property: свойство говорит
*что сделать*, адресат — *где*. Для событий заранее **не обещаем** единую статическую
таблицу: контракт допускает профиль устройства и декодер payload.

### 4.3. Правило разделения

```text
ha_model     — имя, тип значения, единица, диапазон, UI-подсказка.  НИЧЕГО про ZCL.
нормализация — cluster/attr/zcl_type/signed/scale/offset и таблицы mapping. ЗНАЕТ оба мира.
zigbee       — радио, ZDO/ZCL, интервью, binding. Использует нормализацию.
потребители  — семантика (Property/Action/Event). ZCL не импортируют.
```

### 4.4. Общий runtime value (`ha_value_t`)

У `bridge_decode()` должен быть **один** контракт результата, иначе Automation/Display/Web
заведут по своему представлению и семантика снова размножится. Вводим маленький тип:

```c
typedef enum { HA_VALUE_NONE, HA_VALUE_BOOL, HA_VALUE_I32, HA_VALUE_U32,
               HA_VALUE_FLOAT, HA_VALUE_ENUM } ha_value_kind_t;

typedef struct {
    ha_value_kind_t kind;
    union { bool b; int32_t i; uint32_t u; float f; uint32_t e; } v;
} ha_value_t;
```

Поверх него: Automation сравнивает, Display форматирует (по `ha_property_desc`), Web
сериализует. Тип фиксированного размера, без логики, живёт в `ha_model` (форма близка к
`domain_value_t`; сведение к одному типу — отдельный вопрос, `ha_model` не линкует Domain).
Решается **до фазы 1**.

### 4.5. Обратный маппинг: `property → physical key`

Потребителю нельзя самому собирать соседние ZCL-ключи (например, «взять X и Y того же
объекта»). Поэтому мост даёт обратную операцию в том же физическом контексте:

```c
bool semantics_property_key(const ha_zb_state_key_t *context, /* device/endpoint — opaque */
                            ha_property_id_t property,
                            ha_zb_state_key_t *out);
```

Display говорит «дай мне `HA_PROPERTY_COLOR_X` для этого же объекта», а не «замени `attr` на
`0x0003`». На шаге A реализация сменится на `entity_id/property_id`, а потребитель не
изменится. Это и закрывает цветовой виджет (X/Y/яркость одного объекта).

### 4.6. Команды: свойство + действие + значение

Команда — семантический запрос `target (device/endpoint) + property + action + value`, который
мост один раз кодирует в `ha_zb_command_t` (`semantics_build_command`):

```text
POWER            + ON / OFF / TOGGLE
BRIGHTNESS       + SET(%)       COLOR_HUE + SET(°)
COLOR_SATURATION + SET(%)       COLOR_TEMPERATURE + SET(K)
COLOR            + SET(x, y)    ← цвет физически задаётся двумя числами
```

Значение команды — `ha_command_value_t`: `SCALAR` (числовой вид) или `XY` (пара). Не всякая
команда сводится к одному скаляру, поэтому форма значения явная, а не `args[]` наружу.

`COLOR_X`/`COLOR_Y` остаются **свойствами состояния** (что прислало устройство); команда
адресует `COLOR` — командную capability. Семантическая модель не обязана зеркалить raw-атрибуты.

Транспортная кодировка (`scale`, `transition time`, `direction`, LE-порядок, `args_len`)
целиком в мосте. Потребитель передаёт человеческие единицы (%, °, K, xy 0..1).

**Automation.** Персистентная запись правила пока хранит физическую команду
(`action_cluster_id/command_id/args`) — это transitional physical address/payload. Мост **не**
перекодирует её на каждом срабатывании: `automation_rule_command()` копирует запись как opaque.
ZCL↔semantic для сохранённых действий — шаг A. Новые правила из UI позже кодируются через мост
один раз — в старую физическую запись.

## 5. Где живёт маппинг (уточнение к первоначальной идее)

В наброске было `zigbee_property_decode()` прямо из Automation. Так делать нельзя:
Services не зависят друг от друга (`ARCHITECTURE.md` §1.4). Если Automation вызовет
функцию `zigbee`, появится связь Automation → Zigbee.

Поэтому маппинг ZCL ↔ семантика выносится в **отдельный общий модуль-мост**
(рабочее имя `semantics`, финальное — открытый вопрос §10). Его линкуют и `zigbee`
(кодирование/декодирование), и потребители (resolve/decode). Это и есть та самая
«нормализация/binding» из целевой схемы; она принадлежит границе Zigbee, но не радио.

```text
ha_model            словарь смысла (transport-agnostic)
        ▲
        │
semantics (мост)    ZCL ↔ Property/Action/Event; единственный знает cluster/attr
        ▲   ▲
        │   └────────────── consumers (Automation, Display, Web)
        └────────────────── zigbee (радио)

ha_zigbee.h         доступен только мосту и zigbee (в идеале переезжает в мост)
```

## 6. Почему не меняем canonical identity сейчас (миграции)

Смена ключа состояния на `(entity_id, property_id)` — это **смена идентичности таблицы**,
а не правка полей:

```text
изменился sizeof(record) / форма ключа
    → новая геометрия таблицы
    → mstore отдаёт INVALID_STATE (нет миграции)
    → требуется erase раздела (RECORD_MODEL.md §3; mstore_storage_flash.c:127,694-695)
```

Что ещё потянул бы вариант A:

- **WS-провод** — сырой, по offset'ам (`web/src/web.c:55-95`, `web-ui/src/schema.js`,
  `proto.js`, `store.js`); смена ключа = версия протокола v2 → v3 и переделка почти всего
  фронта.
- **group_item** вкладывает state-ключ как под-ключ (`ha_groups.h:44-47`).
- **automation** condition/STATE-триггер/action — ZCL-координаты (`ha_automation.h:71-128`),
  и это **FLASH** (persist_key `automation`).
- **system-девайс** живёт на том же ключе (`ha_system.h:21-37`, `system.c:90-102`) —
  переводить тоже.
- **потеря данных**: erase вычистит устройства, правила, экраны, локацию.

Итого A — это «событие, а не правка» (`RECORD_MODEL.md` §3): большая разовая потеря данных
и протокольная революция ради абстракции, у которой **пока один потребитель — Zigbee**.

**Вывод:** B сейчас не требует ни миграции данных, ни erase, ни смены провода. Записи
остаются прежней геометрии; resolve `cluster/attr → property` делается во время выполнения,
а не хранится. Меняется только семантика и то, кто её знает.

Миграции (и erase) станут неизбежны только на шаге A — и это осознанное решение под
второй транспорт.

## 7. Мост к варианту A

B выбран так, чтобы A перестал быть архитектурной революцией:

```text
сейчас (B):   physical_ref (uid/ep/cluster/attr)  +  semantic property
потом  (A):   canonical_ref (entity_id/property_id) + semantic property
```

Если потребители уже говорят на `Property/Action/Event` (фазы B), при переходе к A
меняется **только адресация** (`physical_ref → entity_ref`), а словарь Automation/UI/Agent
сохраняется. A выполняется вместе с появлением второго транспорта (Matter/Wi-Fi/GPIO),
когда граница окупается и проверяется на реальном втором драйвере.

## 8. Что НЕ делаем (non-goals B)

```text
не меняем canonical state key и его размер          (это шаг A)
не меняем storage geometry и не стираем раздел
не поднимаем версию WS-провода                      (допустимо аддитивно, если понадобится)
не храним property_id в персистентных записях       (resolve — в рантайме)
не вводим TLV / «мешок атрибутов» / самоописание    (RECORD_MODEL §2)
не строим «на будущее» под Matter — только мост под текущий Zigbee
```

## 9. Фазы (кратко)

Подробно — `PROPERTY_MODEL_IMPL_JOURNAL.md`. Каждая фаза заканчивается чекпоинтом
(сборка + host-тесты + проверка на P4 + коммит; откат — revert, формат хранения не меняется
до шага A).

```text
0. Словарь Property/Action/Event + дескрипторы + ha_value_t в ha_model  (аддитивно)
1. Мост: zb_property_map, единый bridge_decode(...) -> ha_value_t
2. Потребители state переходят на property (automation runtime, display widget/format)
3. Команды: Property + Action; вызов моста даёт ha_zb_command_t, постится существующий
   HA_CMD_ZIGBEE_CLUSTER (Domain не меняется)
4. События: HA_EVENT_*, маппер с профилем устройства/payload-декодером
5. Web-адаптер отдаёт семантику в UI; фронт теряет ZCL-словарь
── A (позже): canonical identity (entity_id, property_id) + erase + WS v3
```

## 10. Решённое и открытые вопросы

Уже решено (см. §3–§4):

```text
invariant B          consumer переносит physical ref как opaque address, но не ветвится
                     по cluster/attr/command
zb_property_map      (cluster,attr) → property + scale/offset + optional expected_type;
                     декодирование по state->zcl_type; signedness не нужен
ha_value_t           единый runtime-результат bridge_decode (см. §4.4)
command path (фаза3) мост → ha_zb_command_t → существующий HA_CMD_ZIGBEE_CLUSTER
action target        (device_uid, endpoint) задаёт вызывающий, не property-mapping
event mapper         допускает профиль устройства + payload-декодер, не одну таблицу
attr_scale           уезжает в мост (scale — свойство ZCL-кодировки)
```

Открыто:

1. **Имя/расположение модуля-моста** (`semantics` / `ha_property` / `zcl_map`), что линкует
   `ha_model`, `zigbee` и потребителей, но не Domain.
2. **Переезд `ha_zigbee.h`** из `ha_model` в мост (чтобы ZCL-словарь физически не лежал в
   общем словаре смысла). Проверить, кто ещё импортирует `ha_zigbee.h` кроме моста/zigbee.
3. **Полнота `value_kind`/`unit`**: перечислить единицы (celsius, percent, kelvin, mired,
   lux, volt, none) и дефолт для неизвестных пар.
4. **Отношение к `zcl_type`** в `ha_zb_state_record_t` после B (потребителям он нужен только
   через мост) — решить на фазе 2.
5. **`HA_PROPERTY_UNKNOWN`**: как показывать неизвестные пары в UI/display (деградация без
   ложной семантики).
6. **Action для `SET`**: как выражать «установить в значение» vs `ON/OFF/TOGGLE` (единый
   `SET` + property value, или отдельные действия).
7. **`ha_value_t` vs `domain_value_t`**: свести ли к одному типу (форма близка), с учётом
   того что `ha_model` не линкует Domain.
8. **Профиль события** (device/model + payload-декодер): форма таблицы и как задаётся
   (по модели устройства / по cluster+command).
