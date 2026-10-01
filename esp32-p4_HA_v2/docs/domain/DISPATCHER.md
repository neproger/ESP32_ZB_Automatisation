# Dispatcher

Слой: **Domain / Dispatcher**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Dispatcher — отдельная задача-потребитель Journal. Он доставляет подписчикам compact
события и не выполняет их логику.

- Producer, изменив состояние, **не вызывает подписчиков** — он только пишет state и
  факт и возвращается.
- Dispatcher читает Journal и доставляет события подписчикам.
- Подписчики не выполняются ни в контексте producer'а, ни в контексте Dispatcher'а.

## 2. Доставка

```text
Dispatcher
  читает Journal по event_id (свой cursor)
  для каждого события — находит подходящих подписчиков
  → кладёт compact domain_event_t в inbox/FIFO подписчика
  → будит его task
  продолжает сразу
```

- Доставляется **сам** `domain_event_t` (compact, без больших payload), а не пустой
  wake-up и не только `event_id`.
- Dispatcher не ждёт обработки и не выполняет бизнес-логику.
- Свой inbox/backpressure подписчик организует сам; переполнение его inbox — его
  локальная потеря.

## 3. Порядок и единственность

- События идут по `event_id`; Dispatcher — единственный потребитель Journal.
- Поэтому Journal-gap (см. `JOURNAL.md`) обнаруживает именно Dispatcher.
- Нет head-of-line blocking писателей: медленный подписчик тормозит только себя.

## 4. Что подписчик делает с событием

```text
subscriber task просыпается с event
  ├─ state fact  → читает актуальный state через Domain (get/list)
  ├─ EVENT       → value / domain_payload_get(payload_ref)
  └─ при необходимости → domain_post(command)
```

- Событие — это trigger/context; источник истины для state — Entity Store.
- Подписчик не читает Journal напрямую через хранилище.

## 5. Принятые решения первой версии

**Фильтрация — в Dispatcher.** Иначе он будит вообще всех на каждый факт. Подписчик
передаёт фильтр при подписке:

```text
filter { kind_mask?, source_mask?, entity? }   // 0 / пустая маска — «любые»
```

**Пробуждение — сигнал, не polling.** После успешного `journal_append()` Domain
подаёт внутренний сигнал Dispatcher'у (примитив порта). Dispatcher не опрашивает
Journal по таймеру.

**Inbox принадлежит подписчику.** Domain не создаёт очередь и не решает политику
переполнения. Он знает только delivery-контакт подписчика:

```text
try_push(event, ctx) -> bool    // false — inbox полон, это локальная потеря сервиса
wake(ctx)                       // разбудить задачу подписчика
```

Модель целиком:

```text
producer:   append Journal -> signal
Dispatcher: wait -> читает Journal от cursor -> фильтр -> try_push -> wake -> continue
subscriber: владеет inbox, задачей и политикой переполнения
```

**Dispatcher читает Journal только через внутренний Journal API.** Публичного
`domain_journal_read()` для сервисов нет и не планируется: сервис получает факты через
подписку, а не чтением журнала (ср. открытый вопрос про историю для UI в
`DOMAIN_API.md §12`).

**Gap.** Если `cursor < oldest`, Dispatcher фиксирует gap (счётчик/диагностика),
переходит к `oldest` и продолжает: писателей не блокирует (`JOURNAL.md §5`).
