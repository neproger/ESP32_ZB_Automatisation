# Automation Service

Слой: **Services / Automation**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Automation service — подписчик фактов Domain. При срабатывании правила он **постит
команду**, а не дёргает Zigbee напрямую.

- правила хранятся как обычные entity (`automation`) в Domain;
- при срабатывании публикуется `COMMAND` (см. `../domain/COMMANDS.md`);
- детали runtime правил — внутри сервиса.

## 2. Семантика (state-based)

Обычная автоматика работает по **текущему состоянию**:

```text
event (trigger/context)
  → Automation просыпается
  → читает актуальный state из Entity Store
  → оценивает правило
  → при срабатывании: command
```

То есть:

```text
Journal event = причина проснуться
Entity Store  = данные для решения
```

События при этом **не** схлопываются (`../domain/JOURNAL.md`): каждое доставляется и
считается отдельно. Если состояние успело измениться до пробуждения, правило просто
оценивается по актуальному состоянию.

## 3. Event-like события

События, которые нельзя выразить текущим state (`single_press`, `double_press`,
vendor event), публикуются как `EVENT` и обрабатываются через `value` / `payload_ref`
(`../domain/TRANSIENT_PAYLOAD.md`). Это не заставляет все state-события ходить через
payload ring.

## 4. Форма правила

Правило — entity `automation`; ключ — числовой id. Форма — entity-native `ha_rule_t`
(`ha_model/ha_rule.h`); ссылки — `entity_id`, не физические адреса. **Списки**: до
`HA_RULE_TRIGGERS_MAX`=4 триггеров и до `HA_RULE_ACTIONS_MAX`=4 действий на правило;
**любой** триггер запускает **все** действия (условия — общие).

```text
triggers[]  вид триггера + параметры (до 4):
              DEVICE_EVENT — событие Entity: entity (SOURCE — «источник»), event_id
              TIME         — «будильник»: minutes_of_day (0..1439) + weekday_mask (бит 0=Пн..6=Вс)
              STATE        — состояние свойства: entity (SOURCE — источник), property,
                             op, edge, value, value2
conditions  список условий (AND, до 4): entity (SOURCE — источник), property, op, value, value2
actions[]   canonical действия (до 4): entity (SOURCE — источник), property, action,
            value_kind[, SCALAR: kind+value | XY: x+y]
```

`HA_ENTITY_ID_SOURCE` (в триггере, действиях и условиях) означает «то же Entity, что вызвало событие».

**Loop-guard.** STATE-триггер правила, чьё действие адресует то же `(entity, property)`, что и
триггер, пропускается для этого действия (иначе команда → репорт → снова триггер = петля).

**Виды триггера** (`ha_automation_trigger_kind_t`) в одном байте записи; расширяемо.
`TIME` проверяется на минутном тике системной Entity (`SYSTEM.md`): Automation читает
canonical `(entity, property)` системного времени и сверяет с правилом. Дальше условия —
как обычно (AND).

**STATE** срабатывает на изменение canonical `PROPERTY_STATE` (`ENTITY_UPSERTED`).
Условие `property op value` вычисляется по canonical `ha_value_t`; срабатывание
ограничивается фронтом `edge` (`ha_automation_trigger_edge_t`): `ANY` — каждый раз, когда
условие истинно; `RISING` — «ложно → истинно»; `FALLING` — «истинно → ложно». Предыдущее
значение — из кэша состояний Automation по ключу `(entity_id, property)`. `entity` для
STATE обязателен.

**Условия** — сравнение canonical свойств `(entity_id, property)`; операторы `=`, `≠`,
`>`, `<`, `≥`, `≤`, `содержит биты` (`HA_CONDITION_OP_HAS_BITS`), `диапазон`. Значение —
canonical `ha_value_t`: декод из транспорта делает мост внутри zigbee, Automation получает
готовое значение. Если состояния по ключу нет — условие не выполнено.

## 5. Реализация

Компонент `automation` + `automation_rule_sem.c` (чистая логика, host-тест). Подписка —
по canonical **типу факта** (`source_mask 0`, адаптер неизвестен): `EVENT` (entity
`HA_ENTITY_ENTITY`) для `DEVICE_EVENT`/`TIME` и `ENTITY_UPSERTED` (сущность
`PROPERTY_STATE`) для `STATE`. В задаче перебирает правила (`domain_entity_iter`), при
совпадении триггера оценивает условия по canonical состоянию и постит
`domain_post(HA_CMD_PROPERTY, &ha_property_command_t)`; мутирующий API Domain в try_push
не вызывается (`../domain/DOMAIN_API.md` §9).
