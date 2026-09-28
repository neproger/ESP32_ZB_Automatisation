# ESP32 Home Automation v2 — Architecture Draft

> Статус: **черновик, открыт для обсуждения.**
> Это не финальный контракт. Спорные места помечены `(обсуждается)`.

## 1. Принцип

Ядро максимально тупое и универсальное.

> Если слой нельзя объяснить несколькими простыми правилами — слой слишком большой.

Ядро — `Domain`. Domain не знает предметной области. Он умеет только:

- `get / upsert / remove / list`;
- вести тонкий журнал изменений;
- рассылать изменения подписчикам.

Всё знание о том, **как** создать Zigbee-устройство, как его опросить и как удалить,
живёт в сервисах. Сервисы используют Domain через **тот же интерфейс**, что и Web/Display.

Физически в базу пишет только Domain.

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
├── Journal        — что изменилось (трасса)
└── Dispatcher     — уведомление подписчиков
```

Domain **не** знает про:

- pending UI;
- lifecycle команд;
- cause/correlation;
- staging больших payload;
- процедуры создания/удаления устройств;
- связи (relationships) между сущностями.

Domain исполняет **примитивы** (`get/upsert/remove/list/apply`). Бизнес-процедуры
оркеструют сервисы. `(обсуждается)` — как высокоуровневая команда доходит до сервиса.

## 4. Entity Store

Всё постоянное состояние — сущности. У всех единый жизненный цикл:

```text
get(entity, key)
upsert(entity, key, record)
remove(entity, key)
list(entity, filter?)
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

Не framework: никаких pre/post hooks, handler chains, commit-стадий.

### 4.2. Изменение

`upsert/remove` возвращают `changed / inserted / removed`.
Если ничего не изменилось — **ничего не эмитим** (не создаём шум).

## 5. Journal

Тонкий. Только метаинформация:

```text
{
  seq
  ts
  kind        // ENTITY_CHANGED | COMMAND
  entity
  key
  op          // UPSERT | REMOVE | <команда>
  source      // ZIGBEE | UI | AUTOMATION | SYSTEM | ...
}
```

- Payload в журнал **не попадает** — он живёт в Entity Store.
- Подписчику нужны данные → `domain_get(entity, key)`.
- Большие записи (automation до 4 КБ) проблемы не создают: они лежат в store.
- Journal — RAM ring, **не** персистится. Это трасса/диагностика.
- Источник истины — **Entity Store**, не журнал. Это не Event Sourcing.

## 6. Dispatcher

```text
mutate store
   ↓
release lock
   ↓
append journal
   ↓
sync dispatch
```

Правила:

- **никаких subscriber callbacks под micro_db lock;**
- подписчики read-only по отношению к store;
- подписчик может `domain_post(COMMAND)`, но **не** `domain_upsert/remove` из колбэка;
- отдельную queue/task пока не вводим — только если замеры покажут необходимость `(обсуждается)`.

Подписка с фильтром:

```text
kind, entity, source   (+ wildcard)
```

Сервис, которому нужна асинхронность, заводит свою очередь. Domain не угадывает capacity.

## 7. Команды

Команда = **намерение**. Не сущность. Не RPC.

```text
COMMAND { op, target, args (маленькие, фиксированные) }
```

- UI/сервис публикуют команду.
- Journal её фиксирует.
- Сервис-исполнитель делает работу (например, Zigbee-сервис шлёт на C6).
- Команда **не меняет** подтверждённое состояние.
- Реальный факт изменения устройства приходит позже как обычное изменение сущности.
- Корреляции `command → fact` нет. **Pending — локально в UI.**

Domain «исполняет» только примитивы; высокоуровневое исполнение — в сервисах. `(обсуждается)`

## 8. Сервисы

Сервис — клиент Domain, который:

- подписан на нужные события журнала;
- знает процедуру как последовательность generic-вызовов;
- сам **не трогает** базу.

### 8.1. Zigbee-сервис (пример)

Это state-machine, а не транспорт.

```text
JOIN      → интервью (Active_EP / Simple_Desc / Basic)
          → классификация
          → domain_upsert(device) + domain_upsert(endpoint)* + ...
REPORT    → domain_upsert(device_state)
REMOVE    → device.status = LEAVE_REQUESTED
          → send leave → confirm
          → domain_remove(...) generic-вызовами
REBOOT    → status диктует: продолжить или откатить квест
```

- In-flight работа (интервью, скан) — **локальная RAM сервиса**, не store.
- Долгоживущий прогресс квеста — **поле сущности** (`device.status`).

### 8.2. Automation

Подписчик фактов; при совпадении правила публикует команду (не дёргает Zigbee напрямую).

## 9. Связи сущностей

Domain **не знает** отношений. Связи выражаем данными:

- составные ключи: `endpoint = (device_uid, endpoint)`;
- ссылки в полях: `endpoint.device_uid`, `group_item.device_uid/endpoint`;
- списки внутри записи: `endpoint.in_clusters[]/out_clusters[]`; при необходимости `device.endpoint_count`.

Примеры:

- «найти endpoint'ы устройства» → generic `list(ENDPOINT, prefix = device_uid)`;
- «найти родителя кластера» → endpoint, в записи которого кластер лежит.

Никаких relationship-таблиц, которыми управляет Domain.

## 10. Атомарность

Последовательность generic-вызовов наблюдаема по частям (например, каскадное удаление
устройства). Варианты `(обсуждается)`:

- **A.** Принять пошаговую видимость.
- **B.** Один generic-примитив:

```text
domain_apply(ops[], n)
```

Список CRUD-операций под одним lock → один journal record → один dispatch.
Он универсален (не знает предметку), но даёт атомарный коммит каскада/онбординга.

## 11. UI

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

## 12. micro_db

Storage engine, домен-агностичен. Первый этап:

```text
handle
check(generation/version)
read(copy-out)
```

Позже, только по результатам профилирования: zero-copy borrow, flash layout v2.
Детали — в `MICRO_DB_V2_DRAFT.md`.

## 13. gw_proto

Не портируем как есть. Три уровня:

- **framing** (UART SOF/CRC, WS frame) — транспорт;
- **entity records** — payload хранилища;
- **command record** — маленький фиксированный struct.

Один бинарный контракт сохраняем; типы сокращаем и раскладываем по слоям. `(обсуждается)`

## 14. Инварианты

1. Только Domain физически пишет в store.
2. `micro_db` линкует только Domain.
3. Сервисы и UI — клиенты Domain с единым интерфейсом.
4. Journal тонкий; payload только в Entity Store.
5. Мутация эмитит событие только при реальном изменении.
6. Никаких callbacks под micro_db lock; dispatch после release.
7. Подписчики не мутируют store из колбэка.
8. Command не меняет подтверждённое состояние; корреляции нет.
9. Pending — локально в UI.
10. Связи — данные/ключи, а не Domain-relations.
11. Расширение Domain — максимум generic `apply(batch)`.
12. Нет публичной функции без реализации.

## 15. Миграция из v1

```text
micro_db           → развиваем (handle / check / read)
gw_model           → Entity Store (generic CRUD + registry)
gw_model_notify    → Journal
gw_proto_bus       → Dispatcher
rules engine       → сервис-подписчик
gw_zigbee_uart     → Zigbee-сервис (state-machine)
Web snapshot+delta → сохраняем
Display direct     → сохраняем, через handle/check
ui_control_ack     → удаляем; pending уходит в UI
```

## 16. Открытые вопросы

- `domain_apply(batch)`: нужен ли, и что считать «изменением» для journal.
- Как команда доходит до сервиса: подписка на `COMMAND` в Journal или отдельный command-router.
- Нужен ли третий класс записей (transient/stream) помимо entity и command.
- Journal: capacity, политика переполнения, persist или RAM only.
- Порядок между конкурентными producer'ами (кто раньше попадает в журнал).
- Точный состав типов `gw_proto` и что выкидываем.
- micro_db borrow / zero-copy — только после профилирования.
- Формат ошибок/статусов команд без correlation.

## 17. Формула

```text
service knows procedure
        ↓
domain primitives (get/upsert/remove/apply)
        ↓
Entity Store changed
        ↓
Journal (thin)
        ↓
Dispatcher → subscribers
        ↓
service / display / web react
```
