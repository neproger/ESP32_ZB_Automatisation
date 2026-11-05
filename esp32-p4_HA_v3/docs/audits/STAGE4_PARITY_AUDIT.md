# Stage 4 — Final V2 Parity Audit

> Статус: **Complete**. Итоговая проверка по
> [STAGE_4_IMPLEMENTATION_SPEC.md](../plan/STAGE_4_IMPLEMENTATION_SPEC.md).
> Дата: 2026-11-03. Код не рефакторился в рамках этого аудита.

## 1. Итог

| Компонент | Статус | Доказательство |
|---|---|---|
| Canonical rule decoding | Complete | `decode_rule`/`RuleRecord`; 23 host tests |
| Automation Engine | Complete | TIME/STATE/conditions/actions/index tests |
| Event pipeline | Complete | 3 V2 filters, inbox 32, strict fact checks |
| Task ownership | Complete | `ha-automation/src/task.rs`; `ha-app` only `start` |
| COMMAND_SENT metadata | Complete | host test asserts `Value::enum_(ACTION_ON)` |
| `SetAutomationEnabled` | Complete | `ha_automation::set_enabled`; no `record[384]` in `main` |
| P4 smoke | Complete | rules restore, triggers fire, commands post |
| Final V2 parity | Complete | таблица ниже |

## 2. Parity matrix

| Требование V2 | Результат | Evidence |
|---|---|---|
| 456-byte `ha_rule_t` layout | Pass | `ha-model::canonical::RuleRecord`; no manual decoder |
| Counts >4 clamp | Pass | `decode_rule` clamps 3 counters; test |
| Inactive slots ignored | Pass | inactive slots zeroed before canonical decode; test |
| Malformed active rule isolated | Pass | canonical error logged/skipped per rule; tests |
| `enabled != 0` | Pass | normalized to bool; test |
| Unknown `edge` = ANY | Pass | evaluator default; test |
| TIME source from minute event | Pass | `on_minute(source, ...)`; host + P4 rule |
| WEEKDAY_MASK | Pass | reads `PROPERTY_SYSTEM_WEEKDAY_MASK`; P4 time rule fired |
| STATE current Domain value | Pass | task reads `PROPERTY_STATE`; stale-meta host test |
| Conditions/AND/operators | Pass | V2 semantics retained; tests |
| SOURCE resolution | Pass | TIME condition test + task test |
| Multiple actions, no 16 cap | Pass | callback per action; 80-action host test |
| Rule count >16 | Pass | `Vec<Rule>`; 20-rule test |
| Runtime index 256 | Pass | `MAX_INDEX=256`; explicit overflow log |
| 3 filters | Pass | Event/ENTITY_ENTITY, STATE/PROPERTY_STATE, AUTOMATION |
| Inbox capacity 32 | Pass | `INBOX_CAPACITY=32`; contract test |
| Event/STATE key validation | Pass | exact entity/key/source checks in task |
| Task in service crate | Pass | `ha-automation::start`; `ha-app` has no task/subscription |
| COMMAND_SENT ENUM action | Pass | host fact assertion; P4 log `command posted ... action=1` |
| SetAutomationEnabled V2 contract | Pass | canonical helper; raw offset removed |

## 3. P4 smoke

Firmware: latest `debug/ha-app` flashed via
`espflash --partition-table crates/ha-app/partitions.csv`.

Verified:

- worker start: `ha_automation::task: automation: rules=0 index=0`;
- rule restore after reboot: test rules loaded as `rules=2 index=5`;
- TIME rule with `weekday_mask=0x7f` fired on scheduled minutes;
- TIME condition with `SOURCE` passed and produced actions;
- STATE rule on system `PROPERTY_SYSTEM_MINUTES_OF_DAY` fired on current
  state changes;
- four TIME actions in one rule were posted (no 16-action batch);
- command path: `automation: command posted entity=... action=1`;
- final firmware rechecked with a STATE rule: `command posted ... action=1`, then rule removed;
- no Domain/Automation errors; Zigbee executor reported `Expired`/`Failed`
  because the test device did not answer, not because Domain rejected the
  command;
- test rules were removed afterwards; reboot returned to `rules=0 index=0`.

Note: `COMMAND_SENT` ENUM metadata is not printed by the serial logger; it is
covered by the host fact assertion. The P4 log confirms the command action and
the accepted command path.

## 4. Non-blocking differences

- Rust task stack 8192 and default thread priority vs V2 4096/priority 5:
  platform adaptation, no semantic change.
- Malformed active canonical fields are logged/skipped instead of partially
  evaluated; this is the agreed Stage 4 policy.
- Display migration remains separate (Stage 6); no Display files changed.

## 5. Conclusion

Stage 4 Automation is complete on host and P4 smoke. Stage 3 hardware scenarios
remain a separate acceptance gate.