# Stage 4 — Technical Specification: Automation

> Статус: **Complete** (host + P4 smoke; final parity audit passed).
> Основание: [AUTOMATION_LAYER_AUDIT.md](../audits/AUTOMATION_LAYER_AUDIT.md).
> V2 reference: `esp32-p4_HA_v2/automation/`, `docs/services/AUTOMATION.md`,
> `ha_model/include/ha_model/ha_rule.h`.
> Canonical: `crates/ha-model/src/canonical.rs` (`RuleRecord`, `RuleTrigger`,
> `RuleAction`, `RuleCondition`, `RuleCommandValue`).
> Domain: `crates/ha-domain/src/{fact,dispatch,handle}.rs`.

## 1. Цель

Закрыть Stage 4 без изменения canonical ABI, Domain persistence и Display:
привести Automation к поведению V2 и перенести service ownership из
`ha-app` в `ha-automation`.

Display migration не входит в Stage 4. В `main` допускается только замена
raw `record[384]` на ABI helper; Display-файлы, навигация и UI не меняются.

## 2. Инварианты

- Rules — canonical entity `ENTITY_AUTOMATION` с ключом 8 байт и записью
  `AUTOMATION_RECORD_LEN` (456).
- V2 semantics: любой trigger запускает все actions; conditions общие, AND.
- SOURCE означает entity, вызвавшую факт; для TIME это entity минутного
  события (обычно `ENTITY_SYSTEM`).
- Journal event — причина проснуться; current Domain state — данные для
  решения. EVENT не схлопываются.
- Automation не вызывает Zigbee/Web/Display напрямую; только `Domain::post`.

## 3. Canonical ABI

### 3.1 Decode and V2 normalization

`ha-automation` использует только `ha_model::canonical`:

- `RuleRecord::decode(record)` — counts, reserved, command value;
- `RuleTrigger`, `RuleAction`, `RuleCondition`, `RuleCommandValue`;
- `CanonicalCommand`/`CommandValue` для выхода.

Ручные offsets, `u32_at`/`f32_at` и `command_value` в `ha-automation`
удаляются.

Decode pipeline:

1. Проверить длину `RULE_RECORD_LEN` (456).
2. Скопировать запись и применить только те нормализации, которые делает V2
   (таблица ниже).
3. Для canonical decode обнулить **неактивные** trigger/action/condition slots:
   V2 читает только `count` записей и не валидирует хвост.
4. Выполнить `RuleRecord::decode` на нормализованной копии.
5. Ошибка canonical decode — log с id и `skip` **только этой записи**;
   остальные правила загружаются.

#### V2 normalization / clamp

V2 явно клампит только counts; `edge` и `enabled` нормализует семантика.

| Поле | Поведение V2 | Stage 4 |
|---|---|---|
| `trigger_count > 4` | индекс читает первые 4 | clamp до 4, не malformed |
| `action_count > 4` | fire выполняет первые 4 | clamp до 4, не malformed |
| `conditions_count > 4` | conditions проверяет первые 4 | clamp до 4, не malformed |
| `enabled > 1` | `enabled != 0` → true | нормализовать в bool, не malformed |
| `trigger.edge` неизвестен | switch default → `ANY` | нормализовать в `ANY`, не malformed |
| `trigger.kind` неизвестен | индекс создаётся, но handler не матчится | сохранить V2-поведение: trigger игнорируется |
| `compare.op` неизвестен | `compare` возвращает false | сохранить false, правило не пропускать |
| `action.action == 0` | action пропускается | пропустить action, правило не пропускать |
| `action.action` неизвестен, но ≠0 | V2 постит action как есть | сохранить; правило не пропускать |
| `entity/property == 0` | match/action не срабатывает | сохранить V2-поведение, правило не пропускать |
| `minutes_of_day`/`weekday_mask` вне диапазона | V2 не клампит; просто нет match | сохранить, правило не пропускать |

#### Malformed rule (skip only this rule)

Правило считается malformed, если не проходит structural/canonical decode
**активной части**:

- длина записи не 456;
- ненулевые record reserved (`RULE_RESERVED_OFFSET`);
- у активного trigger/action/condition ненулевые reserved;
- у активного `RuleRef`/`RuleCompare` ненулевые reserved;
- активный `RuleCommandValue` имеет неизвестный kind, невалидный scalar kind
  или ненулевой union у `None`;
- любой другой отказ canonical decode активной записи.

Inactive slots в structural validation не входят: они обнуляются перед
canonical decode, как V2 их не читает. Counts >4 — не malformed, а clamp.


### 3.2 Encode enabled

`SetAutomationEnabled` в `main` не пишет `record[384]`.

В `ha-automation` добавить helper:

```rust
pub fn set_enabled(record: &[u8], enabled: bool) -> Result<[u8; AUTOMATION_RECORD_LEN], AbiError>
```

Helper использует тот же decode pipeline (clamp counts, ignore inactive slots),
меняет `enabled`, делает `encode`. Malformed record → ошибка, запись не
меняется. `main` сохраняет результат через существующий `config::put_record`.
Display-код не меняется.

### 3.3 Command conversion

`RuleCommandValue` → `CommandValue` — один helper в `ha-automation`.
Варианты сохраняются: `None`, `Scalar(Value)`, `Xy { x, y }`.
`RuleAction.action` (u8) переходит в `CanonicalCommand.action` (u32).

## 4. Engine

### 4.1 API

Engine не знает Domain и не постит команды. Действия выдаются callback:

```rust
pub trait StateSource {
    fn state(&self, entity: EntityId, property: PropertyId) -> Option<f64>;
}

pub struct RuleEngine { /* rules: Vec<RuleRecord>, index: [IndexEntry; 256], cache */ }

impl RuleEngine {
    pub fn add_rule(&mut self, id: u64, record: RuleRecord) -> bool;
    pub fn build_index(&mut self);
    pub fn on_event(&mut self, source: EntityId, event: EventId,
                    state: &dyn StateSource, emit: &mut dyn FnMut(CanonicalCommand));
    pub fn on_state(&mut self, entity: EntityId, property: PropertyId, value: f64,
                    state: &dyn StateSource, emit: &mut dyn FnMut(CanonicalCommand));
    pub fn on_minute(&mut self, source: EntityId, minutes: u16, weekday_mask: u8,
                     state: &dyn StateSource, emit: &mut dyn FnMut(CanonicalCommand));
}
```

`Vec<RuleRecord>` заменяет `MAX_RULES = 16`; `MAX_INDEX` остаётся 256 как V2.
При переполнении index — явный log, лишние trigger не индексируются.
`Actions`/`MAX_ACTIONS` удаляются: `fire` вызывает `emit` на каждое действие,
поэтому 17+ действий не теряются.

`RuleEngine::boxed` можно оставить как heap allocation, но без unsafe
uninit; `Box::new(Self::new())` достаточно.

### 4.2 TIME

`on_minute` получает `source` от task (entity минутного события).
Сравнение:

```text
trigger.minutes_of_day == minutes
trigger.weekday_mask & weekday_mask != 0
```

`SOURCE` в conditions/actions разрешается в `source`, как V2.
Task читает `PROPERTY_SYSTEM_MINUTES_OF_DAY` и
`PROPERTY_SYSTEM_WEEKDAY_MASK`; `as u16`/`as u8`, без round/clamp.

### 4.3 STATE

Engine хранит previous-value cache по `(entity, property)` (64 слота, overwrite
первого при переполнении, как V2).

- first observation: RISING true, FALLING false, ANY по holds;
- RISING: holds && (!known || !previous_holds);
- FALLING: !holds && known && previous_holds;
- current `value` приходит из task, прочитанного из Domain, не из `fact.value`.

`entity == 0` и `property == 0` не матчатся.

### 4.4 EVENT

- `event.entity == ENTITY_ENTITY`;
- `key.len() == 8`;
- `source_entity != 0`;
- trigger match: `event_id`, fixed entity или SOURCE.
- `EVENT_MINUTE_TICK` идёт в `on_minute(source, ...)`; остальные — в `on_event`.

### 4.5 Conditions/actions/loop guard

- Conditions: `=`, `≠`, `>`, `<`, `≥`, `≤`, `HAS_BITS`, `BETWEEN`;
  отсутствие state — false; AND.
- Action target: SOURCE или fixed; target/property/action 0 запрещены.
- Loop guard: STATE trigger, action на тот же resolved `(entity, property)`,
  пропускается.
- TIME actions с SOURCE используют source минутного события.

## 5. Service/task ownership

### 5.1 Move

`crates/ha-app/src/automation_task.rs` переносится в
`crates/ha-automation/src/task.rs` (или `service.rs`). `ha-app`:

- удаляет `mod automation_task;`;
- удаляет `automation_tx`/`automation_rx`;
- не создаёт Automation `Filter`;
- вызывает `ha_automation::start(domain.clone())`;
- логирует ошибку старта, не паникует.

`ha-automation` добавляет `log` dependency и публичный `start`.

### 5.2 Subscription

Один `sync_channel(32)`, три подписки с clones одного sender:

```text
Filter { kind_mask: FactKind::Event.bit(), source_mask: 0, entity: ENTITY_ENTITY }
Filter { kind_mask: FactKind::EntityUpserted.bit(), source_mask: 0, entity: ENTITY_PROPERTY_STATE }
Filter { kind_mask: EntityUpserted|EntityRemoved, source_mask: 0, entity: ENTITY_AUTOMATION }
```

Любая ошибка `subscribe_channel` возвращается из `start`; task не стартует.
`try_send`/bounded 32 сохраняют V2 non-blocking loss behavior.

### 5.3 Fact handling

- `EntityUpserted|EntityRemoved` + `ENTITY_AUTOMATION` → reload snapshot.
- `EntityUpserted` + `ENTITY_PROPERTY_STATE` + key len `STATE_KEY_LEN` →
  decode key, read current state; отсутствие значения → skip.
- `Event` + `ENTITY_ENTITY` + key len 8 → source/event; minute tick отдельно.
- прочее игнорировать.

Reload: snapshot `ENTITY_AUTOMATION`; key 8, record 456; `RuleRecord::decode`;
invalid record log/skip; затем `build_index`.

### 5.4 Command emit

Для каждого action:

```text
bytes = command.encode()
meta.source = SOURCE_AUTOMATION
meta.value = Value::enum_(command.action)
domain.post(COMMAND_PROPERTY, bytes, None, meta)
```

Ошибка постинга логируется; цикл продолжается. `Value::NONE` для
COMMAND_SENT больше не используется.

### 5.5 Task properties

Rust `std::thread` stack 8192 и default priority остаются platform adaptation.
V2 4096/priority 5 зафиксированы как reference, но не являются semantic
acceptance criterion.

## 6. Tests

### 6.1 Engine unit tests

- ABI: valid roundtrip; counts >4 clamp; inactive garbage ignored; active
  nonzero reserved / invalid command kind skip only that rule; 4/4/4 valid.
- V2-normalization: `enabled > 1` true; unknown edge = ANY; unknown
  kind/op/action/entity/property follow V2, rule not skipped.
- EVENT: fixed entity, SOURCE, wrong event/entity, source 0.
- STATE: RISING/FALLING/ANY, first observation, repeated value, missing
  previous, entity 0.
- TIME: minute/mask, wrong minute/weekday, TIME SOURCE condition/action.
- Conditions: все операторы, BETWEEN, HAS_BITS, missing state.
- Actions: fixed/SOURCE, invalid target/property/action, loop guard.
- Capacity: >16 rules indexed; one fact emits >16 actions without loss;
  index 257 trigger logs and remains bounded.
- Disabled rule not indexed.

### 6.2 Task/service host tests

- Три фильтра: Event/ENTITY_ENTITY, STATE/PROPERTY_STATE,
  AUTOMATION upsert/removed; CommandSent/Error не приходят.
- Channel 32: 33-й факт даёт drop/`subscriber_drops`, task не блокируется.
- Reload add/remove/disabled; invalid rule skipped.
- STATE: fact old value, Domain current value; trigger evaluates current.
- COMMAND_SENT: source automation, `Value::enum_(action)`.
- TIME: action/condition SOURCE resolves to system event source.
- Event key/entity guards.
- Condition reads current Domain state.

### 6.3 Gates

- `cargo test --workspace`
- `tools/check-domain-api.ps1`
- `tools/build-ha-app.ps1`
- P4 smoke: app starts, `automation: rules=... index=...`, no panic; real
  Stage 3 hardware scenarios остаются отдельным gate.

## 7. Acceptance criteria

- No manual 456-byte offsets or command-value decode in `ha-automation`/`main`.
- Automation task/inbox lives in `ha-automation`; `ha-app` only starts it.
- Three V2 subscriptions share one 32-slot inbox; no `Filter::all()`.
- TIME uses `WEEKDAY_MASK` and SOURCE resolves to event source.
- STATE evaluates current Domain state, not the queued fact value.
- COMMAND_SENT carries ENUM action metadata.
- >16 rules and >16 actions are not silently dropped; index cap 256 is explicit.
- Host tests cover engine, task/inbox, command metadata, reload, drops.
- No changes to canonical ABI, Domain persistence, Zigbee/Device, or Display.

## 8. Out of scope

- Full Display migration and Display audit: Stage 6.
- Wi-Fi/System/Web changes.
- Zigbee/Device/P4 changes.
- New rule features/trigger kinds/persistence.