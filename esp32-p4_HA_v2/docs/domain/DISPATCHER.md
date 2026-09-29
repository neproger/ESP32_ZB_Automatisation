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

## 5. Открытые вопросы

- фильтрация (`kind/entity/source`) на стороне Dispatcher или подписчика;
- механизм пробуждения Dispatcher при появлении нового факта;
- владение inbox подписчика и политика его переполнения.
