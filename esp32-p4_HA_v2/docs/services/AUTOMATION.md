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

Правило — entity `automation`; ключ — числовой id. Форма — в
`ha_model/include/ha_model/ha_automation.h`:

```text
trigger     вид триггера + параметры:
              DEVICE_EVENT — событие от устройства: device_uid (0 — любое), command_id (0 — любая)
              TIME         — «будильник»: minutes_of_day (0..1439) + weekday_mask (бит 0=Пн..6=Вс)
              STATE        — состояние атрибута: device_uid (≠0), endpoint (0 — любой),
                             cluster, attr, op, value, edge
conditions  список условий (AND): device_uid (0 — устройство-источник), endpoint (0 — любой),
            cluster, attr, op, value
action      Zigbee-команда: device_uid (0 — устройство-источник), endpoint, cluster, command, args
```

**Виды триггера** (`ha_automation_trigger_kind_t`) в одном байте записи; расширяемо.
`TIME` проверяется на минутном тике системного девайса (`SYSTEM.md` §4): Automation
читает `minutes_of_day`/`weekday_mask` из состояний системного девайса и сверяет с
правилом (`automation_rule_time_matches`). Дальше условия — как обычно (AND).

`device_uid == 0` (в действии и в условии) означает «то же устройство, что вызвало событие».

**STATE** срабатывает на изменение состояния (`ENTITY_UPSERTED`, сущность `STATE`),
а не на событие-команду. Условие `attr op value` вычисляется по декодированному
значению (как в условиях), и срабатывание ограничивается фронтом `edge`
(`ha_automation_trigger_edge_t`): `ANY` — каждый раз, когда условие истинно;
`RISING` — только переход «ложно → истинно»; `FALLING` — «истинно → ложно».
Предыдущее значение берётся из кэша последних состояний Automation по ключу
`device_uid/endpoint/cluster/attr`; при первом наблюдении (`RISING`/`FALLING`)
правило срабатывает, если условие уже истинно (соответственно ложно). `device_uid`
для STATE обязателен (0 — правило не совпадает).

**Условия** (`ha_automation_condition_t`) — это сравнение **текущего состояния**
атрибута, а не другого события: тот же числовой ключ, что в Entity Store
(`cluster`, `attr`, `endpoint`), плюс оператор (`ha_condition_op_t`) и числовой
порог. Все условия соединяются по И. Значение состояния декодируется по `zcl_type`
(скаляры: bool/uint/int/enum/single float); знаковые приходят расширенными по знаку
(`zigbee_radio.c:report_value`). Если состояния по ключу нет или тип не в словаре
скаляров — условие не выполнено, правило не срабатывает. Операторы: `=`, `≠`, `>`,
`<`, `≥`, `≤` (как в v1) и `содержит биты` (`HA_CONDITION_OP_HAS_BITS`) для наборов —
им удобно проверять маску дней недели системного устройства (`SYSTEM.md` §4).

## 5. Реализация

Компонент `automation` (только IDF) + `automation_rule.c` (чистая логика, host-тест
`test_rule`). Сервис подписан на два факта: `EVENT` от `ZIGBEE` (`kind_mask`,
`source_mask`) — для `DEVICE_EVENT`/`TIME`, и `ENTITY_UPSERTED` c сущностью `STATE` —
для `STATE` (значение атрибута читается прямо из пакета, ставится в кэш). В задаче
перебирает правила (`domain_entity_iter`), при совпадении триггера оценивает
условия по состоянию (`domain_entity_get(HA_ENTITY_STATE, ...)`) и постит команду
(`domain_post(HA_CMD_ZIGBEE_CLUSTER, ...)`); мутирующий API Domain в try_push не
вызывается (`../domain/DOMAIN_API.md` §9).

**Временное правило bring-up.** `main/app_main.c` при старте засевает правило id=1
«нажатие кнопки → toggle реле (EP2) того же устройства» — чтобы сквозной путь был
проверяем до появления UI/Web. Идемпотентно (создаётся, если записи нет); убрать, когда
правила начнёт создавать UI.
