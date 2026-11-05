# Аудит Automation Layer: V2 → Rust

> Статус: **Stage 4 — complete; final parity audit passed.**
> V2: `esp32-p4_HA_v2/automation/`, `docs/services/AUTOMATION.md`,
> `ha_model/include/ha_model/ha_rule.h`, `automation_rule_sem.c`.
> Rust: `crates/ha-automation`, `crates/ha-app/src/automation_task.rs`,
> `crates/ha-app/src/main.rs`, `crates/ha-model/src/canonical.rs`,
> `crates/ha-domain/src/{fact,dispatch,handle}.rs`.

## 1. Следующий слой по зависимостям

По плану следующий слой — **Automation**. Зависимости: Domain и `ha-model`.
Он не зависит от Zigbee, Device/P4, Wi-Fi, System, Web или Display.
P4 boot уже resolved и не является зависимостью этого слоя.

Полная миграция Display не выполнена: сделан только Wi-Fi cleanup
(WIFI_SCAN/WIFI_STATUS через Domain, без локального scan cache и прямого
Wi-Fi runtime). Ответственность Display, навигация и полный Domain-контракт
остаются на Stage 6; в Stage 4 Display не менять.

## 2. Что уже есть

- `RuleEngine`: EVENT/TIME/STATE triggers, conditions, edge detection,
  loop guard, bounded actions, rule index.
- `automation_task`: channel from Domain, state reader, rule reload,
  command publisher.
- V2-семантика в целом совпадает: rules are canonical entities; event/state/time
  triggers; AND conditions; command producer; source-relative references.
- 5 unit tests in `ha-automation` cover basic trigger/condition/loop cases.

## 3. Сверка V2 / ha-model / Domain

| Gap | V2 | ha-model | Domain/current | Решение |
|---|---|---|---|---|
| 456-byte rule | `ha_rule_t`: offsets 0/128/256/384, counts 385–387, name 392 | `RuleRecord`: те же 456 bytes и offsets; decode активной части, counts clamp до 4 | `AUTOMATION_RECORD_LEN` | `RuleRecord::decode/encode` |
| Trigger kinds | EVENT/STATE/TIME | `RuleTrigger.kind` | — | сохранить значения V2 |
| Compare ops | EQ/NE/GT/LT/GE/LE/HAS_BITS/BETWEEN | `RuleCompare.op` | — | сохранить все операторы |
| SOURCE | resolve в source; TIME source = entity тика | `RuleRef.entity` | `ENTITY_ID_SOURCE` | передавать source в `on_minute` |
| STATE source | читает current `PROPERTY_STATE` | `StateRecord`/`StateKey` | `FactKind::EntityUpserted`, `ENTITY_PROPERTY_STATE` | читать current state, не `fact.value` |
| TIME source | `WEEKDAY_MASK` | `PROPERTY_SYSTEM_WEEKDAY_MASK` | System properties | читать mask напрямую |
| Subscriptions | 3 filters, one 32 inbox | — | `Filter {kind_mask, source_mask, entity}`; `try_send` | 3 filters + one 32 channel |
| Command meta | ENUM action | `Value::enum_` | `FactMeta.value` | `Value::enum_(command.action)` |
| Rule count | no cap; index 256 | `RuleRecord` | — | `Vec<RuleRecord>`, index 256 |
| Actions | post each immediately | `RuleAction` | `Domain::post` | callback, no batch cap |

Severity: **P0** — ломает поведение правила; **P1** — архитектура/контракт;
**P2** — совместимость или эксплуатационный риск.

### P0 — TIME-триггер теряет SOURCE

V2 `automation_event_trigger` передаёт в `automation_fire` source entity
минутного события; поэтому `HA_ENTITY_ID_SOURCE` в условии/действии
разрешается в System. Rust `RuleEngine::on_minute` передаёт `source = 0`
(`crates/ha-automation/src/lib.rs`), после чего `resolve(SOURCE, 0) == 0`,
условие не проходит, а действие с `SOURCE` пропускается.

Исправление: `on_minute(source, minutes, weekday_mask, ...)`; task передаёт
entity минутного события (обычно `ENTITY_SYSTEM`), как V2.

### P0 — STATE использует значение факта, а не текущее состояние Domain

V2 `automation_state_trigger` после пробуждения читает актуальную
`PROPERTY_STATE` запись и только потом проверяет STATE-триггер.
Rust `handle_fact` берёт `fact.value`; при отставании inbox правило
оценивает устаревшее значение.

Исправление: task читает current state через `StateSource`/`entity_get`,
передаёт его в engine; `fact.value` использовать только как сигнал
`(entity, property)`, не как источник истины.

### P0 — TIME читает WEEKDAY вместо WEEKDAY_MASK

V2 `read_system_time` читает `PROPERTY_SYSTEM_MINUTES_OF_DAY` и
`PROPERTY_SYSTEM_WEEKDAY_MASK` напрямую. Rust `system_minute` читает
`PROPERTY_SYSTEM_WEEKDAY` и сдвигает `1 << weekday`.

Исправление: читать `PROPERTY_SYSTEM_WEEKDAY_MASK`; часы/маску приводить
как V2 (`as u16`/`as u8`, без round/clamp). V2 `automation_rule_sem_time_matches`
сверяет `trigger.weekday_mask & weekday_mask`.

### P0 — подписка шире V2 и inbox больше

V2 `automation_start` делает три подписки с `source_mask = 0`:

1. `EVENT`, entity `ENTITY_ENTITY`;
2. `ENTITY_UPSERTED`, entity `PROPERTY_STATE`;
3. `ENTITY_UPSERTED|ENTITY_REMOVED`, entity `AUTOMATION`.

Rust `main` подписывает Automation на `Filter::all()` и создаёт канал 64.
V2 inbox — 32 (`AUTOMATION_INBOX_LENGTH`); `xQueueSend(..., 0)` не блокирует
и теряет факт при переполнении.

Исправление: три `subscribe_channel` с одним `SyncSender`/каналом 32;
exact entity/kind filters; `try_send` оставить, drops считать Domain.
Так Automation не получает `CommandSent`/`Error` и чужие entity.

### P0 — COMMAND_SENT теряет action metadata

V2 `automation_fire` ставит `meta.source = DOMAIN_SOURCE_AUTOMATION`,
`meta.value.type = ENUM`, `meta.value.u32 = command.action`.
Rust `automation_task::emit` ставит `Value::NONE`.

Исправление: `FactMeta { source: SOURCE_AUTOMATION as u8, value: Value::enum_(command.action) }`.
Это audit fact, но он входит в V2-контракт команды.

### P0 — правило после 16 теряется

Rust `MAX_RULES = 16`; `add_rule` молча игнорирует запись, если массив полон.
V2 не имеет отдельного cap на число правил: `domain_entity_iter` обходит все,
runtime index ограничен 256 trigger-записями. 17-е правило в Rust просто
не работает.

Исправление: хранить правила в `Vec<RuleRecord>` (или ином unbounded
контейнере), сохранить `MAX_INDEX = 256` и явный log при переполнении
индекса. Не вводить новый произвольный лимит правил.

### P0 — batch actions может потерять команды

`Actions` ограничен 16 (`MAX_ACTIONS`). Один факт может совпасть с несколькими
trigger-записями (несколько правил/триггеров), и V2 постит каждое действие
сразу. При >16 действиях Rust молча теряет команды.

Исправление: убрать batch-cap; `RuleEngine::fire` вызывает sink/`emit` для
каждого действия, как V2 `domain_post` в цикле. Ошибка постинга логируется,
следующее действие продолжает выполняться.

### P1 — canonical ABI продублирован

`ha-model` уже содержит `RuleRecord`, `RuleTrigger`, `RuleAction`,
`RuleCondition`, `RuleRef`, `RuleCommandValue`, offsets and lengths
(`crates/ha-model/src/canonical.rs:1114-1475`). `ha-automation` вручную
декодирует 456-byte record (`Rule::decode`) и command value
(`command_value`), обрезая counts через `.min(4)` и игнорируя reserved.

Исправление: V2-compatible decode pipeline: clamp `trigger_count` /
`action_count` / `conditions_count` до 4, игнорировать inactive slots, затем
`RuleRecord::decode` активной части. Malformed active record — log + skip
только этой записи; остальные правила загружаются. Unknown
kind/op/action/entity/property не считать malformed: для них сохраняется
V2-поведение; unknown `edge` нормализуется в `ANY`. Подробная таблица —
`STAGE_4_IMPLEMENTATION_SPEC.md` §3.1.
`RuleCommandValue` конвертировать в `CommandValue` одним helper в
`ha-automation` (без изменения layout).

### P1 — automation task живёт в `ha-app`

`automation_task.rs` владеет inbox, state reads, reload и command posting.
Это service behavior, а не composition root. `ha-app` должен только
запустить сервис и передать Domain.

Целевое владение:

- `ha-automation` — engine, task/inbox lifecycle, state reader, reload,
  canonical command producer, COMMAND_SENT metadata;
- `ha-app` — `ha_automation::start(domain.clone())`, без automation channel,
  `Filter`, raw record parsing и `SetAutomationEnabled` offsets.

### P1 — Event/STATE факты валидируются не как V2

V2 task принимает Event только для `entity == ENTITY_ENTITY` и
`key_size == sizeof(ha_entity_id_t)`; STATE — только `PROPERTY_STATE` и
`key_size == sizeof(ha_state_key_t)`. Rust Event branch не проверяет entity,
принимает `key.len() >= 8`; STATE принимает `key.len() >= 16`.
V2 semantic Event/STATE matcher также отклоняет `entity == NONE` и
`SOURCE` без источника; Rust `on_event`/`on_state` этого не делают.

Исправление: exact entity/key checks в task и source != 0 в engine.

### P1 — нет task/inbox host tests

`ha-automation` has engine tests; `automation_task` has none. Need host tests for:

- event/state/time trigger through inbox;
- current-state read, not fact value;
- command posted with source/action metadata;
- conditions read current Domain state;
- dropped/full inbox behavior;
- reload on automation add/remove/disabled;
- edge and loop guard through service path;
- TIME `SOURCE` action/condition;
- >16 actions without loss.

### P2 — enabled write uses raw offset

`main::handle_intake` пишет `record[384]` для `SetAutomationEnabled`.
Добавить ABI helper в `ha-automation` (decode `RuleRecord`, set enabled,
encode) и вызывать его из `main`; Display-код не менять. Полный Display
migration остаётся Stage 6.

### P2 — task stack/priority

V2: stack 4096, priority 5. Rust `std::thread` использует 8192 и default
priority. Это platform adaptation, не semantic gap; зафиксировать в spec и
не менять без причины.

## 4. Ownership target

```text
ha-app
  composition: platform, Domain, service registration, thread spawn

ha-automation
  rule engine
  task/inbox lifecycle
  state reader / rule reload
  canonical command producer
  COMMAND_SENT metadata

Domain
  canonical facts/state and command executor
```

`ha-app` remains a thin firmware entry point; it must not own automation
business logic or raw rule parsing.

## 5. Порядок Stage 4

1. Canonical rule ABI: `RuleRecord`/`RuleCommandValue`; V2 count clamp + malformed skip.
2. Engine semantics: TIME source, current state, weekday mask, no action cap,
   `MAX_INDEX=256`, V2 event/state guards.
3. Move task/inbox into `ha-automation`; three V2 filters + channel 32.
4. Fix COMMAND_SENT metadata and `SetAutomationEnabled` ABI helper.
5. Add engine + task/inbox/command-path host tests.
6. Remove automation raw handling from `main`; keep Display untouched.
7. Host gates and P4 smoke; Stage 3 hardware scenarios remain separate.

Detailed implementation contract: [STAGE_4_IMPLEMENTATION_SPEC.md](../plan/STAGE_4_IMPLEMENTATION_SPEC.md).

## 6. Out of scope

- Full Display migration, Display navigation, widgets, touch.
- Zigbee/Device/P4 changes.
- Wi-Fi, System, Web, Display.
- New rule features, new trigger kinds, new persistence.
- Changing canonical ABI, Domain persistence or storage layout.