# Аудит Automation Layer: V2 → Rust

> Статус: **Stage 4, аудит перед реализацией**. Код не менялся.
> V2: `esp32-p4_HA_v2/automation/`, `docs/services/AUTOMATION.md`.
> Rust: `crates/ha-automation`, `crates/ha-app/src/automation_task.rs`,
> `crates/ha-app/src/main.rs`, `crates/ha-model/src/canonical.rs`.

## 1. Следующий слой по зависимостям

По плану следующий слой — **Automation**. Зависимости: Domain и `ha-model`.
Он не зависит от Zigbee, Device/P4, Wi-Fi, System, Web или Display.
P4 boot уже resolved и не является зависимостью этого слоя.

## 2. Что уже есть

- `RuleEngine`: EVENT/TIME/STATE triggers, conditions, edge detection,
  loop guard, bounded actions, rule index.
- `automation_task`: bounded channel from Domain, state reader, rule reload,
  command publisher.
- V2 semantics в целом совпадают: rules are canonical entities; event/state/time
  triggers; AND conditions; command producer; source-relative references.
- 5 unit tests in `ha-automation` cover basic trigger/condition/loop cases.

## 3. Расхождения

### P1 — canonical ABI duplicated in `ha-automation`

`ha-model` already exposes `RuleRecord`, `RuleTrigger`, `RuleAction`,
`RuleCondition`, `RuleRef`, `RuleCommandValue`, offsets and lengths
(`crates/ha-model/src/canonical.rs:1114-1460`). `ha-automation` still decodes
the 456-byte record manually (`Rule::decode` at `lib.rs:385-446`) and manually
decodes command values (`command_value` at `lib.rs:522-544`).

This duplicates the ABI and silently clamps/defaults malformed records. It is
the main Stage 4 cleanup: decode via `RuleRecord::decode` and use the canonical
types. Decide explicitly whether malformed records should be rejected (canonical
decode) or clamped (current behavior); V2 behavior must be the reference.

### P1 — automation task lives in `ha-app`

`automation_task.rs` owns inbox, state reads, rule reload and command posting.
That is automation service behavior, not composition-root behavior. Target:

- `ha-app` starts the service and supplies Domain;
- `ha-automation` owns the task/inbox and command path;
- `main.rs` does not parse automation records or handle `SetAutomationEnabled`.

The move also makes the task host-testable.

### P1 — COMMAND_SENT metadata loses action

V2 `automation_fire` sets `meta.value.type = ENUM` and `u32 = command.action`
(`automation.c:190-205`). Current `automation_task::emit` sets `Value::NONE`
(`automation_task.rs:57-70`). This changes the audit fact, not command semantics.
Use the canonical action metadata when posting `COMMAND_PROPERTY`.

### P1 — subscription is broader than V2

`main` subscribes Automation with `Filter::all()` (`main.rs:91`). V2 subscribes
to the fact types it needs: EVENT, ENTITY_UPSERTED for PROPERTY_STATE, and
AUTOMATION changes. Narrow the filter and preserve bounded drop behavior.
Current channel capacity is 64; V2 inbox is 32. Treat capacity change as an
explicit decision and test drops.

### P1 — no task/inbox host tests

`ha-automation` has engine tests; `automation_task` has none. Need host tests for:

- event/state/time trigger through the task inbox;
- command posted with correct source/action metadata;
- conditions read current Domain state;
- dropped/full inbox behavior;
- reload on automation add/remove;
- edge and loop guard through the service path.

### P2 — time trigger reads weekday instead of weekday mask

`automation_task::system_minute` reads `PROPERTY_SYSTEM_WEEKDAY` and shifts
(`automation_task.rs:74-88`). V2 reads `PROPERTY_SYSTEM_WEEKDAY_MASK` directly
(`automation.c:126-135`). Both properties are published, but the contract should
match V2 and avoid a second interpretation.

### P2 — enabled write uses raw offset

`main::handle_intake` writes `record[384]` for `SetAutomationEnabled`
(`main.rs:318-340`). Use canonical `RuleRecord`/field encoding or an Automation
service helper; do not keep a second ABI decoder in main.

### P2 — actions batch capacity

`ha-automation` bounds a fact’s action batch to 16. V2 posts each action
immediately per fired trigger; there is no equivalent batch limit. Determine
whether 16 can drop valid actions for a rule with multiple triggers and align
with V2 or document the bound.

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

## 5. Proposed Stage 4 order

1. Canonical rule ABI: replace manual decode/encode with `ha-model` types;
   decide malformed-record behavior against V2.
2. Move `automation_task` into `ha-automation`; keep `ha-app` as spawn only.
3. Fix `COMMAND_SENT` metadata and time trigger weekday mask.
4. Narrow subscription and define inbox capacity/drop semantics.
5. Add task/inbox/command path host tests.
6. Remove automation raw record handling from `main`.

## 6. Out of scope

- Zigbee/Device/P4 changes.
- Wi-Fi, System, Web, Display.
- New rule features, new trigger kinds, new persistence.
- Changing canonical ABI or storage layout.