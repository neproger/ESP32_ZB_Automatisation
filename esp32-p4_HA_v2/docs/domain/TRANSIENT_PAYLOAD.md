# Transient Payload

Слой: **Domain / Transient Payload**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Escape hatch для фактов, где компактного `value` недостаточно, payload нужен
runtime-потребителю и **не** является persistent/current state.

Это не обязательный путь для всех событий — только для event-like данных
(vendor-специфичные детали, данные, которые нельзя выразить компактным значением).

## 2. Модель

- Payload хранится в отдельном bounded ring (Ring Store хранилища).
- Сервисы работают не с ring-API, а через тонкий Domain-фасад:

```text
domain_payload_put(payload) → opaque payload_ref
domain_payload_get(ref)     → payload | STALE / NOT_FOUND
```

- Никакого ownership / refcount / release / TTL / consumer tracking: payload живёт,
  пока его не вытеснит ring.
- Вытесненный payload читается как STALE — это нормальная best-effort семантика,
  Domain никого не блокирует и не ждёт.

## 3. Связь с Journal

- Факт несёт опциональный `payload_ref` (см. `JOURNAL.md`).
- `value` (в Journal) и `payload_ref` — независимые identity: payload не ищется через
  `event_id`.
- **Инвариант:** запись Journal осмысленна без payload. `payload_ref` — только
  дополнительная runtime-информация, которую допустимо потерять.

Плохой вариант — когда вся семантика события лежит только в payload; после вытеснения
такая запись бесполезна.

## 4. Открытые вопросы

- критерий «compact value vs transient payload»;
- typed payload (сейчас — generic bytes + size);
- **адресат события:** факт `EVENT` от `domain_payload_put` пока пишется без
  `entity`/`key`. Решать вместе с сервисом-источником событий — тот же вопрос, что и
  для `COMMAND_SENT` (`COMMANDS.md` §7).
