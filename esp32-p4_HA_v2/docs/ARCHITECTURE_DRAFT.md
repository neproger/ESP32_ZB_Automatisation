# ESP32 Home Automation v2 — Architecture Draft

> Статус: **черновик, открыт для обсуждения.**
> Это не финальный контракт. Спорные места помечены `(обсуждается)`.

## 0. Ядро в одной фразе

```text
Entity Store  — хранит данные
Journal       — фиксирует, что произошло (история/диагностика). Не управляет системой.
Dispatcher    — синхронно раздаёт события подписчикам Journal
Command       — transient intent: доставляется напрямую сервису, Journal пишет факт отправки
micro_db      — только механика хранения
```

`Domain` должен оставаться объяснимым ровно этим. Если модели реально не хватает —
поднимаем вопрос отдельно и добавляем **один** механизм, а не наращиваем Domain заранее.

## 1. Принцип

```text
> Если слой нельзя объяснить несколькими простыми правилами — слой слишком большой.
```

`Domain` не знает предметной области. Для него все entity **равны**:
`device`, `automation`, `settings`, `group` — одно и то же. Различается только payload.

`entity_type` нужен только чтобы через простой registry выбрать нужную таблицу/schema.
Если у конкретного типа есть обязательный invariant — допускается validator.
Никаких pre/post hooks, handler chains и скрытого special behavior.

Журнал фиксирует систему, а не управляет ею.

## 2. Слои

```text
                 clients                              services
         Web (WS) | Display            Zigbee | Automation | Settings | System
                 \                          /
                  \                        /
                   v                      v
        ┌──────────────── Domain ─────────────────┐
        │  Entity Store  (generic CRUD)            │
        │  Journal       (thin change log)         │
        │  Dispatcher    (notify subscribers)      │
        └──────────────────────────────────────────┘
                            │
                         micro_db            (приватна для Domain)
```

Правила зависимостей:

- `micro_db` линкует **только** Domain;
- сервисы и клиенты зависят **только** от Domain;
- никто не зависит от конкретного сервиса.

## 3. Domain

```text
Domain
├── Entity Store   — постоянное состояние системы
├── Journal        — что произошло (трасса + канал уведомлений)
└── Dispatcher     — синхронная доставка подписчикам
```

Domain **не** знает про:

- pending UI;
- lifecycle команд;
- cause/correlation;
- staging больших payload;
- процедуры создания/удаления устройств;
- связи (relationships) между сущностями;
- роли, capabilities, permission-модели.

## 4. Entity Store

Всё постоянное состояние — сущности. У всех единый жизненный цикл:

```text
get(entity, key)
list(entity, filter?)
upsert(entity, key, record)
remove(entity, key)
```

Различаются только типы и payload.

```text
device
endpoint
device_state
group
group_item
automation
settings
```

### 4.1. Entity Registry

Простая routing-таблица:

```text
entity_type → { micro_db table/schema, optional validate }
```

Schema типа также задаёт **семантику сравнения**:

```text
record_equals(base)   — изменилась ли запись физически (включая ts)
change_equals(semantic)— изменилось ли смысловое значение
```

Не framework. Никаких pre/post hooks.

### 4.2. Изменение: storage update vs semantic change

Операция записи различает два факта:

```text
updated = true   — запись изменилась физически (например, новый ts)
changed = true   — изменилось смысловое значение
```

Пример: датчик снова прислал `23.5`, но с новым timestamp.

```text
updated = true
changed = false
    ↓
новый ts сохраняется в Entity Store
но Journal НЕ эмитит ENTITY_CHANGED
(нет Automation wakeup, WS delta, UI redraw)
```

Domain остаётся тупым: сравнение задаётся схемой entity, а не Domain.
Journal эмитит событие **только** при `changed = true`.

### 4.3. Изменение через upsert

Пользователь изменил automation — это **не** command workflow и **не** staging:

```text
domain_upsert(AUTOMATION, "auto-1", record)
```

Automation Engine подписан на `ENTITY_CHANGED / AUTOMATION`, делает `domain_get()` и
обновляет своё runtime-состояние. То же для settings, groups, device state.

Большие payload (включая automation на несколько КБ) живут **только** в Entity Store
и никогда не копируются в Journal.

## 5. Journal

Journal — это **одновременно** канал уведомлений об изменениях и системный журнал
для истории/диагностики (в т.ч. дебага автоматик).

Он **не является** транспортом доставки. Команды через него не ходят (см. §7).

Тонкая запись:

```text
{ seq, ts, kind, entity, key, op, source }
```

- Payload в журнал **не попадает** — он живёт в Entity Store.
- Подписчику нужны данные → `domain_get(entity, key)`.
- `kind=ENTITY_CHANGED` — только при `changed = true` (см. §4.2).
- `kind=COMMAND_SENT` — факт успешной передачи команды исполнителю (см. §7).
- Journal — RAM-блоки, capacity compile-time/configurable (старт, например, с 50 событий).
  Блок заполнен → заводим новый, старый пока выбрасываем.
- В будущем старый блок можно асинхронно выгружать во flash/SD как историю, не меняя
  hot-path. **Сейчас эту механику не проектируем.**
- Источник истины — **Entity Store**, не журнал. Это не Event Sourcing.

## 6. Порядок и Dispatcher

Порядок — простой и строгий, last-writer-wins. Конфликт-резолюшн и merge не нужны.

Один инвариант: **изменение Entity Store и запись в Journal имеют один сериализованный
порядок**, и следующий writer не может поменять ту же entity раньше, чем синхронные
подписчики обработали предыдущее событие.

Внешний Domain-lock покрывает **только mutations/publication**:

```text
acquire Domain-lock
    mutate Entity Store        (внутри — короткий micro_db lock)
    if changed: journal append
    sync dispatch              (НЕ под micro_db lock)
release Domain-lock
```

Read API во время dispatch:

```text
domain_get() / domain_list()  НЕ берут Domain-lock.
Они берут только внутреннюю защиту соответствующей micro_db table.
```

Это обязательно, иначе subscriber, вызывающий `domain_get()` внутри callback,
сделает deadlock сам себе.

- subscriber callbacks **никогда** не выполняются под `micro_db` lock;
- очередь и отдельный dispatch task не нужны;
- dispatch двух операций не перемешиваются (Domain-lock держится до конца dispatch).

## 7. Команды

Команда — **transient intent**, а не состояние. Она **не идёт через Journal как канал
доставки** — у неё два независимых аспекта:

```text
delivery path  → напрямую в service/executor
audit path     → тонкая запись в Journal
```

```text
UI / Automation
      ↓
domain_post(command)
      ↓
Domain маршрутизирует команду соответствующему service/executor
      ↓
после успешной передачи → thin record в Journal
      ↓
Journal уведомляет своих subscribers о факте отправки
```

- `kind=COMMAND_SENT`: `target`, `op`, `source`, `ts/seq`.
- Args команды **не хранятся** в Journal и не имеют lifetime-проблемы: их получает
  исполнитель в момент dispatch.
- Если передача не состоялась (нет executor / ошибка) — COMMAND_SENT не пишем.
  При реальной необходимости позже можно ввести отдельный `COMMAND_REJECTED`.
- Отдельного persistent command journal/queue/store нет.
- Большие payload команд не проектируем; вероятно, «тяжёлые изменения» — это
  обычные `upsert` сущностей.

Корреляцию `command → fact` **не отслеживаем**. Это не HTTP RPC.
UI отправил команду → Journal зафиксировал отправку → позже устройство реально
изменилось → Entity Store обновился → Journal зафиксировал **факт изменения**.
UI не сопоставляет эти события, а просто отображает текущее состояние.
Pending — **локальная UI-механика**; Domain о нём не знает.

### Граница: COMMAND vs request/response

```text
COMMAND = fire-and-forget intent
```

Операции, которым вызывающему реально нужен ответ (`read_attr`, network scan,
`permit_join`, binding table и т.п.), — **другой класс взаимодействия**.
Мы его сейчас **не решаем** и не смешиваем с COMMAND.
Позже для него может появиться отдельная минимальная модель.

## 8. Subscribers

Подписчик:

- слушает Journal (фильтр по `kind/entity/source`);
- читает нужную entity через `domain_get()`;
- при необходимости инициировать действие — вызывает `domain_post(command)`.

Роли, capability system и permission-модели сейчас не вводим.

## 9. Связи сущностей

Domain **не знает** отношений. Связи выражаем данными:

- составные ключи: `endpoint = (device_uid, endpoint)`;
- ссылки в полях: `endpoint.device_uid`, `group_item.device_uid/endpoint`;
- списки внутри записи: `endpoint.in_clusters[]/out_clusters[]`; при необходимости `device.endpoint_count`.

Примеры:

- «найти endpoint'ы устройства» → `list(ENDPOINT, prefix = device_uid)`;
- «найти родителя кластера» → endpoint, в записи которого кластер лежит.

### Удаление (REMOVE)

Для `UPSERT` subscriber делает `event → domain_get(key)`.
Для `REMOVE` читать нечего — значит Journal record для удаления **обязан содержать
полный canonical key**, достаточный для реакции без payload. Для составных ключей
(`device + endpoint + cluster/...`) это явный invariant.

Никаких relationship-таблиц, которыми управляет Domain.

## 10. UI

### Display

Читает Entity Store напрямую через Domain, своей модели не держит.
Пользуется `handle/check`, чтобы не читать payload без нужды.
Подписан на `ENTITY_CHANGED`, перечитывает только изменённое.

### Web

Физически удалён, держит реплику:

```text
ENTITY_CHANGED → domain_get → WS → Browser Store → React
```

Снапшот при подключении + дельты дальше.

### Pending

Локальная механика UI. Domain в ней не участвует.

## 11. micro_db

Storage engine, домен-агностичен. Первый этап — не усложняем:

```text
handle
check(generation/version)
read(copy-out)
```

`generation` существует именно для slot reuse — вопрос реализации `check()`,
не архитектурный. Zero-copy borrow, новый flash-layout — **позже, только по замерам**.
Детали — в `MICRO_DB_V2_DRAFT.md`.

## 12. gw_proto

Не переносим как есть. Сохраняем сильную идею — единый бинарный контракт и минимальные
преобразования — но делим ответственность по слоям:

```text
canonical entity records   — типы хранилища / Domain
event/command envelope     — отдельный универсальный слой
transport framing          — UART SOF/CRC, WS frame
```

- Интерфейс (envelope) — одинаковый, payload-структуры — строго типизированные.
- Сокращаем количество специальных типов и `MSG_*` там, где хватает универсального
  интерфейса. `(обсуждается)` — точный список.

## 13. Zigbee

В core-архитектуре специально **не проектируем**.

Фиксируем только: Zigbee будет одним из сервисов/подписчиков Domain, и команды
приходят к нему **напрямую** (см. §7), а не через Journal.
Детали его state machine, адресации, cluster/endpoint-модели и P4↔C6 — отдельная тема.

## 14. Инварианты

1. Только Domain физически пишет в store.
2. `micro_db` линкует только Domain.
3. Сервисы и UI — клиенты Domain с единым интерфейсом.
4. Все entity равны; `entity_type` — только routing, максимум validator.
5. Journal тонкий; payload только в Entity Store.
6. Store change и Journal append — один сериализованный порядок; last-writer-wins.
7. Domain-lock покрывает только mutations/publication; read API берёт только lock таблицы.
8. Никаких callbacks под micro_db lock; dispatch после release storage-lock.
9. Подписчики читают через `domain_get()`, не мутируют store из колбэка.
10. Journal эмитит `ENTITY_CHANGED` только при `changed = true`.
11. Command — fire-and-forget intent; доставляется напрямую сервису; Journal пишет
    только `COMMAND_SENT`; корреляции нет; pending — локально в UI.
12. REMOVE record содержит полный canonical key.
13. Связи — данные/ключи, а не Domain-relations.
14. Domain не расширяем заранее; новый механизм — только под доказанную потребность.
15. Нет публичной функции без реализации.

## 15. Миграция из v1

```text
micro_db           → развиваем (handle / check / read)
gw_model           → Entity Store (generic CRUD + registry)
gw_model_notify    → Journal
gw_proto_bus       → Dispatcher
rules engine       → сервис-подписчик
gw_zigbee_uart     → Zigbee-сервис (state-machine, отдельная тема)
Web snapshot+delta → сохраняем
Display direct     → сохраняем, через handle/check
ui_control_ack     → удаляем; pending уходит в UI
```

## 16. Открытые вопросы

Закрываем **явным решением**, а не новым слоем автоматически.

1. **Семантика semantic change.** Для каких entity types задаём `change_equals`
   и как (сенсоры vs актуаторы). Нужно решить до реализации — сенсоры быстро покажут
   проблему ts-only.
2. **Representation automation trace.** v1 публикует `GW_PROTO_TRACE_*`
   (fired / action / error / action_index). Как это выражается в тонком Journal:
   дополнительный `kind` или отдельный канал.
3. **gw_proto.** Точный список типов на сокращение и формат универсального envelope.
4. **Journal capacity.** Значение по умолчанию и retention (сейчас configurable,
   старт ~50). Persistence — позже.
5. **Request/response.** Отдельная минимальная модель для операций, которым нужен
   ответ. Граница зафиксирована (COMMAND = fire-and-forget), реализация — позже.
6. **COMMAND_REJECTED.** Нужен ли отдельный факт отказа при неудачной передаче команды,
   или достаточно просто не писать COMMAND_SENT.

Уже принято (не открытые):
- порядок и неперемешивание dispatch — закрыто §6 (Domain-lock);
- каскадное удаление — последовательность событий нормальна, атомарность не вводим,
  пока не появится конкретный invariant;
- handle/generation для slot reuse — вопрос реализации `check()`;
- lifetime args команды — закрыто (аргументы у исполнителя в момент dispatch).
