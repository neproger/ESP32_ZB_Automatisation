# Аудит Zigbee Adapter: V2 + Z2M → Rust

> Статус: **Stage 3 — Host Verified / Hardware Blocked (acceptance; P4 boot resolved)**.
> Реализация 3.1–3.5 и host-интеграция пройдены; queue test проходит.
> P4 boot blocker resolved: прошит финальный `debug/ha-app`, приложение
> стартует и подключается к Wi-Fi; остались hardware scenarios.
> V2: `esp32-p4_HA_v2/zigbee/`, `docs/services/ZIGBEE.md`,
> `docs/services/ZIGBEE_CAPABILITIES.md`.
> Rust: `crates/zigbee-devices`, `crates/z2m-rs`, `crates/zigbee-exec`,
> `crates/zigbee-frames`, `crates/ha-app/{zigbee_service,configure,interview,frames,store}.rs`,
> `crates/ha-app/native_zigbee/zigbee_poc.c`.

## 0. Утверждённые решения Stage 3

| Вопрос | Решение |
|---|---|
| `Binding.confirmed` | Отдельное состояние binding, не alias `interview::confirmed`; новая привязка — неподтверждённая; configure/report обновляют binding; restart — без ложного подтверждения |
| Remover mark failure | Возвращать ошибку remover, canonical cleanup не выполнять; `AlreadyAbsent` только при доказанном отсутствии устройства |
| TOGGLE reconciliation | Ожидаемое = противоположное известному On/Off; если состояние неизвестно — не снимать desired, ждать timeout/подтверждение |
| Hue/Sat | Сохранять combined HS V2+Z2M; отдельные V2 hue/sat не возвращать без требования |
| `COLOR_ENHANCED_TO` 0x43 | Добавить явную поддержку в bridge |
| Level/bridge fallback | Неизвестная команда — явная unsupported-ошибка, не подмена на 0x00 |
| `REMOVALS` queue | Bounded queue, явная ошибка переполнения, без silent drop |

## 1. Граница Stage 3

Сохраняем:

- private registry / `mstore`;
- interview;
- deferred leave;
- Zigbee identity (`EUI64`, endpoint);
- ZCL/transport ownership;
- `z2m-rs` profiles/converters and `zigbee-exec` retry/correlation.

Не меняем:

- canonical ABI/record layout;
- Domain persistence contract;
- Device Service lifecycle (Stage 2 API);
- Automation/System/Web/Display.

## 2. Binding lifecycle

### V2

V2 binding — adapter-private mapping `(uid, endpoint) → entity_id`
(`zigbee_entity_binding.c`). Capabilities/actions берутся из endpoint
descriptor; отдельного `confirmed` snapshot в binding нет.

### Rust

`Binding` содержит `confirmed: Confirmed` (`zigbee-devices/src/command.rs:62-70`).
Создание:

```text
zigbee_service::register_domain
  -> State.bindings.push(Binding {
       confirmed: interview::confirmed(eui),
       ...
     })
```

`confirmed` фиксируется в момент регистрации (`zigbee_service.rs:400-408`).

Обновление подтверждений происходит позже:

- `frames::ingest` подтверждает атрибут после report
  (`frames.rs:166`) и отправляет обновлённые capabilities;
- `configure::on_result` подтверждает атрибуты после успешного read
  (`configure.rs:455-472`) и также отправляет capabilities;
- `interview::confirm_attribute` обновляет глобальную таблицу
  `CONFIRMED` (`interview.rs:176-199`).

Но ни один путь не обновляет `Binding.confirmed`. `CommandRouter`
проверяет capabilities по этому старому snapshot
(`command.rs:106-119`).

### Следствия

- после restart binding создаётся с пустым `Confirmed`;
- configure успешно подтверждает атрибуты и обновляет Domain
  capabilities, но команды могут продолжать отклоняться
  `InvalidArg`, пока binding не будет создан заново;
- report также подтверждает атрибут, но binding остаётся прежним.

### Минимальный объём

`Binding.confirmed` остаётся отдельным состоянием binding; нельзя
копировать `interview::confirmed` в binding и нельзя использовать
`interview::confirmed` как замену.

- Новая привязка создаётся с пустым `Confirmed`; регистрация не
  подтверждает атрибуты.
- Успешный configure подтверждает ровно те атрибуты, которые реально
  пришли в read response.
- Report подтверждает binding только для фактически подтверждённого
  атрибута.
- Existing binding не сбрасывается повторной регистрацией.
- После restart binding создаётся пустым и восстанавливается только
  configure/report, без ложного confirmation.

Подтверждения из configure/report должны попадать в `State` явным
событием; `interview::confirmed` обновляется отдельно для capabilities.
Тест: report/read подтверждает атрибут → команда разрешена без
повторной регистрации; повторный restart снова требует configure.

## 3. Command reconciliation

### Фактическое поведение

`PendingCommand.desired` хранит исходный `CommandValue`
(`command_core.rs:52-58`). `on_reported` принимает только
`entity` и `property` (`command_core.rs:101-107`):

```rust
if entry.entity == entity && entry.property == property {
    *slot = None;
}
```

`zigbee_service::apply_state` добавляет `(entity, property)` в
`reported` для каждого decoded state, даже если Domain record не
изменился (`zigbee_service.rs:588-590`). Затем `run` вызывает
`on_reported` без значения (`zigbee_service.rs:196-200`,
`255-259`).

### Расхождения

1. Любой report по той же entity/property снимает desired, даже если
   reported value не совпадает с ожидаемым.
2. ON/OFF command имеет `CommandValue::None`; ожидаемое значение
   bool можно вывести только из `ACTION_ON/OFF`.
3. TOGGLE не имеет предсказуемого ожидаемого значения.
4. COLOR command использует `PROPERTY_COLOR` и `CommandValue::Xy`
   (`projection.rs:326-334`), но reported state приходит как
   `PROPERTY_COLOR_X` и `PROPERTY_COLOR_Y` (`projection.rs:204-211`).
   Поэтому pending color-команда не совпадает ни с одним report и
   живёт до timeout.
5. Read response также вызывает `on_reported`; это допустимо только
   при совпадении значения.

### Минимальный контракт

Нужен явный `Desired`/`Expected` тип в `CommandCore`, derived from
`CanonicalCommand`:

| Action/property | Expected |
|---|---|
| POWER ON/OFF | `POWER = Bool(true/false)` |
| POWER TOGGLE | противоположное известному On/Off; если неизвестно — без снятия по report |
| BRIGHTNESS SET | `BRIGHTNESS = Scalar` |
| COLOR_TEMPERATURE SET | `COLOR_TEMPERATURE = Scalar` |
| COLOR SET XY | `COLOR_X`/`COLOR_Y`; оба должны совпасть |
| неизвестное | не снимать desired |

`on_reported` должен принимать `value` (из `StateUpdate`) и сравнивать
нормализованное значение. Для XY нужен progress по двум осям, иначе
один report снимет команду преждевременно. Для TOGGLE ожидаемое
значение вычисляется как противоположное известному последнему On/Off;
при неизвестном состоянии desired не снимается, пока не сработает
timeout или отдельное подтверждение.

Тесты: exact match, different value, unrelated property, color X/Y
partial/success, toggle, repeated report.

## 4. ZCL transport bridge

### V2 action map

`zigbee_semantics.c:229-243`:

| Property/action | Cluster | ZCL command | Codec |
|---|---|---|---|
| POWER ON | 0x0006 | 0x01 | none |
| POWER OFF | 0x0006 | 0x00 | none |
| POWER TOGGLE | 0x0006 | 0x02 | none |
| BRIGHTNESS SET | 0x0008 | 0x00 | level % |
| COLOR_HUE SET | 0x0300 | 0x00 | hue degrees |
| COLOR_SATURATION SET | 0x0300 | 0x03 | saturation % |
| COLOR_TEMPERATURE SET | 0x0300 | 0x0A | K→mireds |
| COLOR SET XY | 0x0300 | 0x07 | xy |

### Current Z2M/canonical

`projection::canonical_command` поддерживает только
POWER, BRIGHTNESS, COLOR_TEMPERATURE и COLOR XY
(`projection.rs:294-337`). Прямые `PROPERTY_COLOR_HUE`/
`PROPERTY_COLOR_SATURATION` команды отсутствуют.

`z2m-rs` standard commands (`capabilities.rs:297-303`):

- `ONOFF_ON/OFF/TOGGLE`;
- `LEVEL_TO` → 0x0008/0x04 (`moveToLevelWithOnOff`);
- `COLORTEMP_TO` → 0x0300/0x0A;
- `COLOR_XY_TO` → 0x0300/0x07.

Generated profiles additionally use:

- `LEVEL_TO_NO_ONOFF` → 0x0008/0x00;
- `COLOR_HS_TO` → 0x0300/0x06;
- `COLOR_ENHANCED_TO` → 0x0300/0x43;
- `COLOR_XY_TO`, `COLORTEMP_TO`.

### C bridge support

`zb_exec_send_zcl` (`native_zigbee/zigbee_poc.c:493-583`):

- 0x0006: 0x00/0x01/0x02;
- 0x0008: all commands with `len >= 3`; only 0x04 is dispatched to
  `moveToLevelWithOnOff`, every other command falls through to
  `moveToLevel` (0x00);
- 0x0300: 0x0A, 0x07, 0x06;
- no 0x0300/0x43; no 0x0300/0x00 or 0x03.

### Gaps — approved fixes

1. **P1 — enhanced HS:** generated profiles can emit `COLOR_ENHANCED_TO`
   (0x43), but the C bridge rejects it. Add explicit 0x43 support using
   `ezb_zcl_color_control_enhanced_move_to_hue_and_saturation_cmd_req`.
2. **P1 — Level command validation:** C bridge silently treats
   unsupported Level commands as 0x00. It must reject unknown
   cluster/command explicitly, not substitute another command.
3. **P2 — V2 hue/saturation:** keep Z2M combined 0x06. Do not add
   separate V2 0x00/0x03 without a real consumer.
4. **P2 — standard profile command set:** keep combined HS as the
   V2+Z2M contract; `STANDARD_TO` may retain only the commands that are
   actually dispatched by the bridge.
5. **P2 — unsupported error mapping:** C returns `-1` for unsupported;
   `exec.rs` maps it to `SendError::Io`. Map unsupported explicitly so
   it is not retried as a transient I/O error.

### Minimal work

- Add 0x43 to the C bridge, with payload `enhanced_hue u16`,
  `saturation u8`, `transition u16`, `options`.
- Validate Level/Color cluster command ids in the bridge; return
  unsupported for unknown ids.
- Keep Z2M combined HS as primary.
- Map unsupported transport result distinctly if the executor contract
  needs it.

## 5. Private registry, interview, deferred leave

### V2

- private RAM device/endpoint store;
- private entity binding;
- NVS remove mark (max 16) for deferred leave;
- interview applies radio result to private topology;
- on leave success: private device removed, entity forgotten, mark
  cleared.

### Rust

- `Registry<CFlash>` in `mstore`, tombstone `InterviewStatus::Removed`
  (`registry.rs:159-193`);
- `device_addresses` skips removed records (`store.rs:265-285`);
- announce path checks `store::is_removed` and issues leave
  (`zigbee_service.rs:485-501`);
- leave confirmation clears the mark (`zigbee_service.rs:234-240`);
- interview owns `InterviewManager` and descriptor in RAM
  (`interview.rs:123-166`).

This is an accepted Rust adaptation: tombstone in registry replaces a
separate NVS remove list. It preserves V2 semantics and survives reboot.

### Discrepancy — P0/P1

`zigbee_remover` currently does:

```rust
if !store::mark_removed(eui) {
    return Ok(NativeRemoval::AlreadyAbsent);
}
```

(`zigbee_service.rs:141-149`)

`store::mark_removed` returns false when the registry is not mounted or
the write fails (`store.rs:196-210`). V2 returns the mark error
(`zigbee.c:128-136`) and therefore aborts canonical removal. Current
Rust reports success, so Device Service proceeds with canonical cleanup
without a durable deferred-leave mark; an announce can re-add the
device.

Approved fix: propagate `store::mark_removed` error as remover error;
Device service must not perform canonical cleanup. `AlreadyAbsent` is
only for a genuinely absent physical device, not storage failure.

Queue: replace unbounded `REMOVALS: Vec<u64>` with a fixed-capacity
queue, capacity 8 as in V2. `push` returns an explicit error when full;
no request is silently dropped. Mark is written first; on queue overflow
the remover returns the error and canonical cleanup is aborted (the mark
may remain for the announce path).

Other observations:

- `device_addresses` excludes removed records, so restart after a
  deferred removal is correct.
- `interview::confirmed` is RAM-only; this matches V2 and is why
  binding refresh after restart is required.

## 6. Ownership / no-change checks

Current boundary is correct after Stage 2:

- adapter writes `PROPERTY_STATE` via `DomainAdapter::write_state`;
- adapter does not write canonical LogicalDevice/Entity/Capabilities;
- Device Service owns lifecycle;
- private registry/interview/bindings stay in adapter.

Stage 3 must not change these checks. Existing static checks in
`tools/check-domain-api.ps1` already cover no adapter lifecycle writes.

## 7. Recommended Stage 3 order

1. **Binding freshness** — empty on registration; configure/report update
   binding; restart restores without false confirmation.
2. **Remover failure** — propagate mark persistence/queue errors before
   canonical cleanup.
3. **Command reconciliation** — valued `on_reported`, normalized
   ON/OFF/SET/XY/TOGGLE; expected toggle from known state.
4. **ZCL bridge** — 0x43 support, explicit unsupported result, no silent
   0x00 fallback.
5. **REMOVALS queue** — capacity 8, explicit overflow error.
6. **Verification** — tests for restart/repeat configure/report,
   persistence errors, mismatched reports, queue overflow; host checks
   and P4 list.

## 8. Requires P4

- configure → binding confirm → immediate command without restart;
- restart with known devices → configure refresh → command;
- enhanced-HS device if 0x43 is added;
- deferred leave offline/online and reboot;
- 0x0008/0x0006 command matrix on real devices.

Host-only: command core, binding refresh logic, bridge dispatch table,
remover error mapping.

## 9. Decisions status

All four decisions are approved in section 0. No open architecture
decision remains for Stage 3; implementation must follow
[STAGE_3_IMPLEMENTATION_SPEC.md](../plan/STAGE_3_IMPLEMENTATION_SPEC.md).
