# Stage 3 — Technical Specification: Zigbee Adapter

> Статус: **Host Verified / P4 boot verified; hardware scenarios pending**.
> Реализация 3.1–3.5 и host-интеграция 3.6 выполнены; queue unit test
> проходит через `cargo test --workspace`. P4 boot blocker resolved:
> прошит финальный `debug/ha-app`; приложение стартует, Wi-Fi получает
> `192.168.1.100`. Полная приёмка Stage 3 открыта до hardware scenarios.
> Основание: [ZIGBEE_LAYER_AUDIT.md](../audits/ZIGBEE_LAYER_AUDIT.md).
> План: [V2_TO_RUST_PORTING_PLAN.md](V2_TO_RUST_PORTING_PLAN.md).
> Принципы: [V2_TO_RUST_PORTING_PRINCIPLES.md](V2_TO_RUST_PORTING_PRINCIPLES.md).

## 1. Цель

Закрыть Stage 3 без перепроектирования Zigbee subsystem:

- актуальный `Binding.confirmed`;
- корректная ошибка persistence при deferred-leave;
- reconciliation desired/reported, включая TOGGLE;
- явная поддержка ZCL `0x43` и отказ от silent fallback;
- bounded `REMOVALS` queue.

## 2. Незыблемые ограничения

Запрещено менять:

- canonical ABI, record layouts и `ha-model`;
- Domain persistence contract, manifest, backing и storage;
- Device Service lifecycle/removal API из Stage 2;
- private registry format и его сохранение в `mstore`;
- interview/deferred-leave модель;
- Automation, System, Web, Display;
- Zigbee protocol ради «улучшения» V2+Z2M без отдельного решения.

Не добавлять:

- новую canonical table;
- второй persistent state;
- migration/recovery state machine;
- background retry framework;
- unbounded queues.

Разрешённые изменения: `zigbee-devices`, `ha-app` Zigbee service/configure/
interview/frames/store, `ha-app/native_zigbee/zigbee_poc.c`, tests, docs.
`z2m-rs` profiles/encoding не менять, если тест показывает, что encoding
уже корректен.

## 3. Утверждённые решения

| Вопрос | Решение |
|---|---|
| Binding freshness | `Binding.confirmed` — отдельное состояние binding, не alias `interview::confirmed` |
| Remover mark failure | Ошибка возвращается remover; canonical cleanup не выполняется |
| TOGGLE | Ожидаемое = противоположное известному On/Off; неизвестно — не снимать по report |
| Hue/Sat | Сохранять combined HS V2+Z2M; отдельные V2 команды не добавлять |
| `COLOR_ENHANCED_TO` | Явно поддержать `0x43` в bridge |
| Level/bridge fallback | Неизвестная команда → explicit unsupported, не `0x00` |
| `REMOVALS` | Bounded queue capacity 8, explicit overflow error |

## 4. Этап 1 — Binding freshness

### 4.1. Модель

`Binding.confirmed` — confirmation snapshot **этого** binding
(`eui`, `endpoint`). Он не является копией `interview::confirmed`.

`interview::confirmed` остаётся источником для capabilities/projection.
`Binding.confirmed` используется `CommandRouter` для проверки команд.
Оба могут обновляться одним событием, но не читают и не переписывают
друг друга.

### 4.2. Правила

| Событие | Binding.confirmed |
|---|---|
| `register_domain` создаёт новый binding | пустой `Confirmed::new()` |
| `register_domain` видит существующий binding | не сбрасывать |
| restart/restore | новый binding пустой; ложного подтверждения нет |
| configure read подтвердил атрибут | подтвердить этот атрибут в matching binding |
| report подтвердил feature | подтвердить этот атрибут в matching binding, только если cluster является server-кластером этого endpoint |
| registration без configure | не подтверждать |
| `interview::confirmed` изменился | не копировать автоматически |

Matching binding: `binding.eui == eui && binding.endpoint == endpoint`.
Для standard binding report/read может подтвердить только advertised
server-кластер и известный canonical attribute этого кластера. Non-standard
binding и «чужой» cluster/attribute не обновляют `Binding.confirmed`.
`Confirmed::confirm` уже идемпотентен.

### 4.3. Реализация

1. В `zigbee-devices/src/command.rs` добавить:
   ```rust
   impl Binding {
       pub fn confirm(&mut self, cluster: u16, attribute: u16) -> bool {
           self.confirmed.confirm(self.endpoint, cluster, attribute)
       }
   }
   ```
2. В `register_domain` (`ha-app/src/zigbee_service.rs`) заменить
   `confirmed: interview::confirmed(eui)` на `Confirmed::new()`.
   Existing binding не трогать.
3. В `State` добавить:
   ```rust
   fn confirm_binding(&mut self, eui: u64, endpoint: u8, cluster: u16, attribute: u16)
   ```
   Обновляет все matching bindings.
4. `configure::on_result` должен вернуть точные подтверждения, полученные
   из успешного read response, а не перечитывать `interview::confirmed`.
   `zigbee_service::run` применяет их к `State` под уже существующим lock.
5. Report path: `frames::ingest` после `decoded_feature` добавляет
   подтверждение в `Ingest`-буфер; `run` применяет его к `State`.
   `interview::confirm_attribute` остаётся отдельным вызовом для
   capabilities.
6. Не менять `CommandRouter` API: он продолжает использовать
   `Binding.confirmed`.

### 4.4. Тесты

- новый binding пустой, даже если interview уже что-то подтвердил;
- configure read → binding.confirmed содержит атрибут → команда разрешена;
- report → binding.confirmed содержит атрибут;
- report/read не подтверждает чужой endpoint/device;
- повторный register не сбрасывает существующий binding;
- restart-модель: новый binding пустой до configure/report;
- тест в `zigbee-devices/tests/command_router.rs` на `Binding::confirm`.

## 5. Этап 2 — Remover failure и bounded queue

### 5.1. Mark failure

`store::mark_removed` должен возвращать `Result<(), ha_domain::Error>`:
- registry не смонтирован → ошибка;
- запись/commit не удалась → ошибка;
- успех → `Ok(())`.

`zigbee_remover`:

```text
parse native_id -> eui
store::mark_removed(eui)?       // ошибка -> Err, canonical cleanup запрещён
REMOVALS.try_push(eui)?          // overflow -> Err, canonical cleanup запрещён
interview::forget(eui)
configure::forget(eui)
Ok(NativeRemoval::Removed)
```

`AlreadyAbsent` не использовать для ошибки persistence или queue.
Он остаётся допустимым только если remover действительно доказал
физическое отсутствие устройства. Для текущего Zigbee remover это не
является штатным путём.

### 5.2. Queue

Заменить `static REMOVALS: Mutex<Vec<u64>>` на fixed-capacity queue:

- capacity `8` (V2 remove queue length);
- `try_push(eui) -> Result<(), ha_domain::Error>` с `NoSpace` при
  переполнении;
- `take_all() -> ([u64; 8], usize)` или эквивалент без роста;
- Никакого silent drop;
- overflow возвращается вызывающему коду; запрос не теряется; canonical
  cleanup не выполняется.

`remove_device` после успешного canonical cleanup забирает очередь и
выполняет leave для каждого eui, как сейчас. Если canonical cleanup
не удался, очередь не опустошается этим путём.

### 5.3. Тесты

- mark error → remover error, canonical records не меняются;
- queue capacity 8: 8 успешных push, 9-й `NoSpace`;
- successful mark + full queue → canonical cleanup не начинается;
- mark success + queue success → canonical cleanup, leave path;
- restart: removed record не восстанавливается;
- announce для removed устройства уходит в leave.

## 6. Этап 3 — Command reconciliation

### 6.1. Expected

Заменить `PendingCommand.desired: CommandValue` на явное ожидаемое
состояние. Варианты:

```rust
const SCALAR_EPSILON: f32 = 0.01;
const COLOR_XY_EPSILON: f32 = 0.001;

enum Expected {
    Value { property: PropertyId, value: Value },
    Color { x: f32, y: f32, seen_x: bool, seen_y: bool },
    Toggle { expected: Option<bool> },
}
```

Правила:

| Command | Expected |
|---|---|
| POWER ON | `Value(POWER, Bool(true))` |
| POWER OFF | `Value(POWER, Bool(false))` |
| POWER TOGGLE | `Toggle { expected: last_power(entity) }` |
| BRIGHTNESS SET | `Value(BRIGHTNESS, scalar)` |
| COLOR_TEMPERATURE SET | `Value(COLOR_TEMPERATURE, scalar)` |
| COLOR SET XY | `Color { x, y, seen_x:false, seen_y:false }` |
| unsupported | `InvalidArg` до отправки |

Для TOGGLE нужен transient last-reported On/Off. Хранить его только в
`CommandCore` как часть reconciliation, не в Domain и не в registry.
Нет known значения → `expected: None`; report не снимает desired,
работает timeout.

### 6.2. on_reported

Сигнатура:

```rust
pub fn on_reported(&mut self, entity: EntityId, property: PropertyId, value: Value)
```

Алгоритм:

1. Обновить transient last-reported для `(entity, property)`.
2. Для каждого pending:
   - `Value`: снять только если property совпадает и values равны;
     для F32 scalar tolerance `0.01`, для остальных — exact kind/value;
   - `Color`: для `COLOR_X`/`COLOR_Y` обновить соответствующий флаг;
     снять только когда оба флага true и обе оси совпали с ожидаемыми;
     один совпавший report не завершает команду; mismatch сбрасывает флаг;
   - `Toggle`: снять только если `expected == Some(report_bool)`;
     `None` не снимается;
   - unrelated property — не трогать.
3. Report с изменённым/неизменённым Domain record обрабатывается
   одинаково; важна value, не `changed` flag.

`zigbee_service` передаёт `state.value` в `on_reported` для report и read
response.

### 6.3. Тесты

- matching scalar → cleared;
- different scalar → not cleared;
- unrelated property → not cleared;
- F32 tolerance;
- TOGGLE known current → opposite clears, same does not;
- TOGGLE unknown → no report clears; timeout clears;
- color X only → not cleared; X+Y match → cleared; mismatch resets;
- duplicate reports;
- pending replacement same entity/property.

## 7. Этап 4 — ZCL bridge

### 7.1. Dispatch matrix

`zb_exec_send_zcl` must accept only:

| Cluster | Command | Action |
|---|---|---|
| 0x0006 | 0x00/0x01/0x02 | On/Off/Toggle |
| 0x0008 | 0x00 | MoveToLevel |
| 0x0008 | 0x04 | MoveToLevelWithOnOff |
| 0x0300 | 0x06 | MoveToHueAndSaturation |
| 0x0300 | 0x07 | MoveToColor |
| 0x0300 | 0x0A | MoveToColorTemperature |
| 0x0300 | 0x43 | EnhancedMoveToHueAndSaturation |

Other cluster/command → explicit unsupported; never substitute 0x00.

### 7.2. 0x43

Use SDK:

```c
ezb_zcl_color_control_enhanced_move_to_hue_and_saturation_cmd_t req;
req.payload.enhanced_hue = u16_le(payload[0..2]);
req.payload.saturation   = payload[2];
req.payload.transition_time = u16_le(payload[3..5]);
req.payload.options_mask     = len >= 7 ? payload[5] : 0;
req.payload.options_override = len >= 7 ? payload[6] : 0;
ezb_zcl_color_control_enhanced_move_to_hue_and_saturation_cmd_req(&req);
```

Validate `len >= 5` (or exact encoder length) before dispatch.

### 7.3. Errors

C bridge must distinguish:

- `0` — accepted;
- `unsupported` — cluster/command/length not in matrix;
- `io` — SDK call failed.

`exec.rs` maps unsupported to `SendError::Unsupported`, not `Io`, so the
executor does not retry a permanently unsupported command as transient.
Keep the public `SendError` contract.

### 7.4. Tests

- `z2m-rs` already tests 0x43 encoding; keep/extend if needed;
- host test for `exec.rs` mapping if a testable seam exists;
- P4 command matrix for 0x06/0x07/0x0A/0x43 and Level 0x00/0x04;
- ensure no silent fallback for 0x01/0x02 Level and unknown Color.

## 8. Этап 5 — Verification

Обязательные команды:

```text
cargo test --workspace
cargo test -p zigbee-devices
cargo test -p ha-device
tools/check-domain-api.ps1
tools/build-ha-app.ps1
git diff --check
```

Дополнительно вручную проверить:

- adapter не пишет canonical LogicalDevice/Entity/Capabilities;
- `Binding.confirmed` не читает `interview::confirmed`;
- remover не вызывается под Domain mutex;
- queue не растёт и не теряет элементы молча;
- `0x43` и Level dispatch не подменяют команду.

## 9. Verification status

| Проверка | Статус |
|---|---|
| `cargo test --workspace` | Пройдено |
| `check-domain-api.ps1` | Пройдено |
| `build-ha-app.ps1` | Пройдено |
| Binding/remover/reconciliation/ZCL/queue implementation | Выполнено |
| Queue unit test | **Пройден** через `crates/zigbee-devices/tests/removal_queue.rs` (host module include) |
| P4 hardware validation | **Boot resolved**: flashed `debug/ha-app` (not `libespidf.bin`); app starts and Wi-Fi connects. Scenarios pending |
| Stage 3 acceptance | **Не закрыт**: hardware scenarios pending |

## 10. Acceptance criteria

- Новый binding пустой; configure/report обновляют только binding.
- После restart binding пустой до configure/report; ложного подтверждения нет.
- Команда разрешается после реального configure/report.
- Mark persistence error возвращается, canonical state не меняется.
- Queue capacity 8; 9-й push даёт explicit error.
- Desired снимается только при match; TOGGLE использует known state.
- Color desired снимается только после X+Y match.
- `0x43` поддержан; unknown Level/Color команда — unsupported.
- Все host tests/build/static checks зелёные.
- Ни одного изменения canonical ABI, Device lifecycle, registry format,
  Automation/System/Web/Display.

## 11. P4-проверки после реализации

Boot resolved: см. [STAGE3_HARDWARE_VALIDATION.md](../audits/STAGE3_HARDWARE_VALIDATION.md).

- configure → binding confirm → immediate command;
- restart known device → configure → command;
- enhanced HS device (0x43);
- Level 0x00 and 0x04;
- report mismatch/duplicate and toggle;
- deferred leave online/offline/reboot;
- queue overflow behavior with multiple removals.

## 12. Запрет на расширение scope

Если при реализации обнаружится необходимость изменить canonical ABI,
Domain persistence, Device lifecycle, private registry layout, profiles,
или другой слой — остановиться и вынести отдельное решение. Не
«исправлять» симптомы в обход этого ТЗ.
