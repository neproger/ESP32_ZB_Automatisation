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
Automation / Display / Web / Agent — работают только с семантикой
```

Ключевой тезис: **ZCL остаётся identity физического состояния, но перестаёт быть его
семантикой для верхних слоёв.**

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
typedef struct {
    uint16_t         cluster_id;
    uint16_t         attr_id;
    ha_property_id_t property;
    uint8_t          zcl_type;
    uint8_t          signedness;
    float            scale;    /* ZCL-encoding: 0.01 °C и т.п. */
    float            offset;
} zb_property_map_t;
```

Плюс симметричные таблицы для действий и событий:

```text
действие:  (property, action)  → (cluster_id, command_id, args encoding)
событие:   (cluster_id, command_id|attr) → HA_EVENT_* и его параметры
```

### 4.3. Правило разделения

```text
ha_model     — имя, тип значения, единица, диапазон, UI-подсказка.  НИЧЕГО про ZCL.
нормализация — cluster/attr/zcl_type/signed/scale/offset и таблицы mapping. ЗНАЕТ оба мира.
zigbee       — радио, ZDO/ZCL, интервью, binding. Использует нормализацию.
потребители  — семантика (Property/Action/Event). ZCL не импортируют.
```

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
0. Словарь Property/Action/Event + дескрипторы в ha_model   (аддитивно, без поведения)
1. Мост: zb_property_map, единое декодирование state
2. Потребители state переходят на property (automation runtime, display widget/format)
3. Команды: Property + Action, zb_action_map
4. События: HA_EVENT_* и маппинг сырых ZCL-событий
5. Web-адаптер отдаёт семантику в UI; фронт теряет ZCL-словарь
── A (позже): canonical identity (entity_id, property_id) + erase + WS v3
```

## 10. Открытые вопросы

1. **Имя/расположение модуля-моста** (`semantics` / `ha_property` / `zcl_map`), что линкует
   `ha_model`, `zigbee` и потребителей, но не Domain.
2. **Переезд `ha_zigbee.h`** из `ha_model` в мост (чтобы ZCL-словарь физически не лежал в
   общем словаре смысла). Проверить, кто ещё импортирует `ha_zigbee.h` кроме моста/zigbee.
3. **Полнота `value_kind`/`unit`**: перечислить единицы (celsius, percent, kelvin, mired,
   lux, volt, none) и дефолт для неизвестных пар.
4. **Отношение к `zcl_type`**: остаётся ли `zcl_type` в `ha_zb_state_record_t` после B
   (потребителям он нужен только через мост) — решить на фазе 2.
5. **`HA_PROPERTY_UNKNOWN`**: как показывать неизвестные пары в UI/display (деградация без
   ложной семантики).
6. **Action для `SET`**: как выражать «установить в значение» vs `ON/OFF/TOGGLE` в общем
   словаре (единый `SET` + property value, или отдельные действия).
7. **Судьба `attr_scale`**: полностью уезжает в мост (scale — свойство ZCL-кодировки), в
   `ha_model` не остаётся.
