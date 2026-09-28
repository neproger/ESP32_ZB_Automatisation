# ESP32 Home Automation v2 — Architecture Draft

> Статус: **черновик, открыт для обсуждения.**
> Это не финальный контракт. Спорные места помечены `(обсуждается)`.

## 0. Ядро в одной фразе

```text
Entity Store  — хранит данные
Journal       — фиксирует, что произошло, и служит каналом уведомлений
Dispatcher    — синхронно раздаёт события подписчикам
Command       — transient intent: Domain передаёт исполнителю и логирует факт отправки
micro_db      — только механика хранения
```

`Domain` должен оставаться объяснимым ровно этим. Если модели реально не хватает —
поднимаем вопрос отдельно и добавляем **один** механизм, а не наращиваем Domain заранее.

## 1. Принцип

Ядро максимально тупое и универсальное.

> Если слой нельзя объяснить несколькими простыми правилами — слой слишком большой.

`Domain` не знает предметной области. Для него все entity **равны**:
`device`, `automation`, `settings`, `group` — одно и то же. Различается только payload.

`entity_type` нужен только чтобы через простой registry выбрать нужную таблицу/schema.
Если у конкретного типа есть обязательный invariant — допускается validator.
Никаких pre/post hooks, handler chains и скрытого special behavior.

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
├── Journal        — что изменилось (трасса + канал уведомлений)
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

Не framework.

### 4.2. Изменение

`upsert/remove` возвращают `changed / inserted / removed`.
Если ничего не изменилось — событие **не** эмитим.

Пример: пользователь изменил automation. Это **не** command workflow и **не** staging:

```text
domain_upsert(AUTOMATION, "auto-1", record)
```

Automation Engine подписан на `ENTITY_CHANGED / AUTOMATION`, делает `domain_get()` и
обновляет своё runtime-состояние. То же для settings, groups, device state.

Большие payload (включая automation на несколько КБ) живут **только** в Entity Store
и никогда не копируются в Journal.

## 5. Journal

Тонкий. Только метаинформация:

```text
{ seq, ts, kind, entity, key, op, source }
```

- Journal — это **одновременно** канал событий и системный журнал для диагностики
  (в т.ч. дебага автоматик).
- Payload в журнал **не попадает** — он живёт в Entity Store.
- Подписчику нужны данные → `domain_get(entity, key)`.
- Journal — небольшие RAM-блоки (например, ~50 событий). Блок заполнен → заводим новый,
  старый пока выбрасываем.
- В будущем старый блок можно асинхронно выгружать во flash/SD как историю, не меняя
  hot-path. **Сейчас эту механику не проектируем.**
- Источник истины — **Entity Store**, не журнал. Это не Event Sourcing.

## 6. Порядок и Dispatcher

Порядок — простой и строгий, last-writer-wins. Конфликт-резолюшн и merge не нужны.

Один инвариант: **изменение Entity Store и запись в Journal имеют один сериализованный
порядок**, и следующий writer не может поменять ту же entity раньше, чем синхронные
подписчики обработали предыдущее событие (если они читают payload через `domain_get()`).

Решение — простой внешний Domain-lock на всю доменную операцию:

```text
acquire Domain-lock
    mutate Entity Store      (внутри — короткий micro_db lock)
    if changed: journal append
    sync dispatch            (НЕ под micro_db lock)
release Domain-lock
```

- subscriber callbacks **никогда** не выполняются под `micro_db` lock;
- очередь и отдельный dispatch task для этого не нужны;
- если в будущем появится медленный подписчик, вопрос решается отдельно `(обсуждается)`.

## 7. Команды

Команда — **transient intent**, а не состояние.

```text
domain_post(command)
    ↓
Domain синхронно передаёт команду заинтересованному subscriber/executor
    ↓
в Event Journal пишется только факт отправки:
target, operation, source, timestamp/seq, короткая мета
```

- Команда **не обязана** храниться как полноценная запись Entity Store.
- Небольшие фиксированные `args` могут жить **только** во время synchronous dispatch
  и вообще не попадать в исторический Journal.
  (Если исполнитель асинхронен, он сам копирует args в свою очередь `(обсуждается)`.)
- Отдельного persistent command journal/queue/store на старте **нет**.
- Большие payload команд сейчас не проектируем; вероятно, «тяжёлые изменения» — это
  вообще не команды, а обычные `upsert` сущностей.
- Если позже выяснится, что команды надо буферизовать/повторять/гарантированно
  доставлять — это отдельная доказанная потребность.

Корреляцию `command → fact` **не отслеживаем**. Это не HTTP RPC.
UI отправил команду → Journal зафиксировал отправку → позже устройство реально
изменилось → Entity Store обновился → Journal зафиксировал уже **факт изменения**.
UI не сопоставляет эти события, а просто отображает текущее состояние.
Pending, если нужен конкретному контролу, — **локальная UI-механика**. Domain о нём не знает.

## 8. Subscribers

Подписчик:

- слушает Journal (фильтр по `kind/entity/source`);
- читает нужную entity через `domain_get()`;
- при необходимости инициировать действие — отправляет `COMMAND`.

Роли, capability system и permission-модели сейчас не вводим — лишнее.

## 9. Связи сущностей

Domain **не знает** отношений. Связи выражаем данными:

- составные ключи: `endpoint = (device_uid, endpoint)`;
- ссылки в полях: `endpoint.device_uid`, `group_item.device_uid/endpoint`;
- списки внутри записи: `endpoint.in_clusters[]/out_clusters[]`; при необходимости `device.endpoint_count`.

Примеры:

- «найти endpoint'ы устройства» → `list(ENDPOINT, prefix = device_uid)`;
- «найти родителя кластера» → endpoint, в записи которого кластер лежит.

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

Этого достаточно, чтобы Display перестал постоянно читать payload.
Zero-copy borrow, новый flash-layout и прочие оптимизации — **позже, только по замерам**.
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

Фиксируем только: Zigbee будет одним из сервисов/подписчиков Domain.
Детали его state machine, адресации, cluster/endpoint-модели и P4↔C6 — отдельная тема.

## 14. Инварианты

1. Только Domain физически пишет в store.
2. `micro_db` линкует только Domain.
3. Сервисы и UI — клиенты Domain с единым интерфейсом.
4. Все entity равны; `entity_type` — только routing, максимум validator.
5. Journal тонкий; payload только в Entity Store.
6. Store change и Journal append — один сериализованный порядок; last-writer-wins.
7. Никаких callbacks под micro_db lock; dispatch после release storage-lock.
8. Подписчики читают через `domain_get()`, не мутируют store из колбэка.
9. Command — transient intent; корреляции нет; pending — локально в UI.
10. Связи — данные/ключи, а не Domain-relations.
11. Domain не расширяем заранее; новый механизм — только под доказанную потребность.
12. Нет публичной функции без реализации.

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

Это места, где простая модель может не стыковаться с реальной реализацией.
Закрываем **явным решением**, а не новым слоем автоматически.

1. **ts-only изменения.** Считать ли `changed`, если изменился только timestamp
   (сенсоры шлют тот же value с новым ts)? Иначе — churn в Journal.
2. **Глобальный Domain-lock при sync dispatch.** Медленный подписчик блокирует
   **всех** писателей, а не только по той же entity. Приемлемо ли, или подписчик
   обязан иметь собственную очередь?
3. **Reentrancy.** Subscriber отправляет COMMAND прямо внутри dispatch, когда Domain-lock
   удержан. Нужно правило: `domain_post` не должен брать store/Domain-lock (иначе рекурсия).
4. **Request/response операции.** `read_attr`, `permit_join`, network scan, binding table,
   factory reset, device remove confirm — результат нужен, а correlation нет.
   Куда приходит результат и кто адресат? (в v1 — `CMD_RESULT`).
5. **Automation trace.** v1 публикует `GW_PROTO_TRACE_*` (fired / action / error /
   action_index). Как это выражается в тонком Journal: дополнительный `kind` или
   отдельный канал?
6. **Каскадное удаление** (device → endpoints / state / group_item / meta). Несколько
   `domain_remove`, наблюдаемая неконсистентность и возможный interleave.
   `domain_apply(batch)` сейчас **не вводим**; обсуждаем только при конкретном invariant.
7. **Journal capacity.** ~50 записей достаточно для Events-страницы и дебага автоматик?
   Или нужен больший retention / несколько блоков уже сейчас?
8. **`list(prefix)` для связей.** Нужен ли частичный индекс в micro_db, или допускаем
   полный scan таблицы (например, `endpoint`)?
9. **gw_proto.** Точный список типов на сокращение и формат универсального envelope.
10. **micro_db handle и slot reuse.** Как Display надёжно обнаруживает REMOVE/STALE
    при переиспользовании слота.
11. **Command args при асинхронном исполнителе.** Подтвердить правило: копирует args
    в свою очередь сам исполнитель.
12. **Порядок Journal == порядок store** при нескольких producer'ах — гарантируется
    Domain-lock; подтвердить, что перемешивание dispatch между двумя операциями допустимо.
