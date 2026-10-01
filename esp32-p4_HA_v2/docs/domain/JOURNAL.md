# Journal

Слой: **Domain / Journal**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Journal — **системный поток фактов**: короткая история + источник событий для
подписчиков. В нём только то, что уже произошло.

Journal **не** является транспортом доставки: команды через него не ходят (см.
`COMMANDS.md`). Источник истины — Entity Store, не Journal. Это не Event Sourcing.

## 2. Запись

`domain_event_t` — compact runtime-descriptor факта, он же запись Journal:

```text
event_id    — identity этой Journal-записи (opaque доменный тип)
ts
kind        — ENTITY_UPSERTED | ENTITY_REMOVED | EVENT | COMMAND_SENT
op          — UPSERT | REMOVE | <command op>
source      — ZIGBEE | UI | AUTOMATION | SYSTEM | ...
entity, key
value       — optional компактный snapshot (history / diagnostics)
payload_ref — optional ссылка на Transient Payload (см. TRANSIENT_PAYLOAD.md)
```

Категории:

```text
ENTITY_UPSERTED / ENTITY_REMOVED  — изменился persistent/current state
EVENT                             — что-то произошло, state из этого не следует
COMMAND_SENT                      — Domain передал intent исполнителю
```

Одна и та же структура живёт в Journal и доставляется подписчикам — отдельного
event-lookup нет.

## 3. Что хранится

> Journal **никогда** не хранит большие entity records. Он может хранить компактный
> snapshot значения, необходимый для понимания исторического факта.

Причина: `domain_get()` возвращает только последнее состояние; без значения событие
со временем теряет смысл.

- Компактное значение — маленький variant (`NONE / BOOL / I32 / U32 / F32 / ENUM`).
- Human-readable строки не храним: это форматирование/локализация/heap; Events UI
  строит читаемый вид из структуры.
- Если компактного `value` мало и payload нужен runtime-потребителю, факт несёт
  `payload_ref` (Transient Payload).
- **Инвариант:** запись Journal осмысленна без payload; `payload_ref` — только
  дополнительная runtime-информация.

## 4. Retention

- Journal — bounded ring: при заполнении вытесняется самая старая запись.
- Никто факт не удерживает и не ждёт: если consumer не успел прочитать — потеря
  принимается (Zigbee-политика).
- События **не** схлопываются: каждый факт — отдельная запись (`on on on off` —
  четыре факта).
- Длинная история / persistence — не проектируется.

**Append в штатном пути не отказывает.** Journal не валидирует и не принимает решений:
при заполнении ring вытесняет oldest. Ошибка `append` означает нарушение внутреннего
контракта (Journal не инициализирован, `key_size` типа шире лимита записи, исчерпан
технический счётчик) — аварийная ветка, а не бизнес-исход. Поэтому «состояние записано,
а факт не добавился» не образует отдельной семантики partial commit: откатывать
mstore-транзакцию нечем, и `domain_entity_put()` не превращается в транзакцию ради
гипотетического отказа Journal.

## 5. Gap

Dispatcher — единственный consumer Journal, поэтому Journal-gap обнаруживает именно он:
ожидаемый `event_id` меньше `oldest` в ring. Тогда он пишет diagnostic/counter,
продолжает с oldest available и не блокирует producer. Локальное переполнение inbox
конкретного сервиса — потеря этого сервиса, а не Journal-gap.

## 6. Открытые вопросы

- критерий `value` vs `payload_ref`;
- нужна ли длинная история/persistence;
- размер ring.
