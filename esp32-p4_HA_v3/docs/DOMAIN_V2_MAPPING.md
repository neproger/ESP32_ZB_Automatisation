# HA Domain V2 ↔ z2m-rs mapping audit

Sources: V2 `domain/include`, `ha_model/include`, `zigbee/src/zigbee_semantics.c`,
`zigbee/include/zigbee/zigbee_entity_binding.h`; V3 `ha-domain`.

## Layering (V2, to preserve)

- **Domain** (`domain`) is a generic fact/entity store + command dispatch. It knows
  opaque entity/command numbers, keys and fixed records; it does not know Zigbee.
- **ha_model** defines the canonical vocabulary: entity identity, properties,
  value kinds, capabilities, logical devices, state records.
- **zigbee semantics/binding** is the only place that knows ZCL clusters,
  attributes, endpoints, seeds and command encoding.
- A `Profile` never belongs in Domain. It is adapter data.

## Canonical types and exact numbers

- `ha_entity_id_t` u64; derived with SipHash-2-4 over
  `LE32(issuer) || LE32(seed_len) || seed`. Zigbee issuer = **1**.
  Zigbee seed = **EUI64 LE (8) + endpoint (1) = 9 bytes**.
  `0` is none; all-ones is the automation source sentinel.
- `ha_device_id_t` u64; derived from `source` (Zigbee source = **1**) and
  `native_id` (lowercase EUI hex). Device record:
  `parent(8) source(4) native_id[32] name[32] reserved[4]` = 80 bytes.
- Entity record: `device_id(8) name[32]` = 40 bytes.
- Property ids: POWER 1, BRIGHTNESS 2, COLOR_HUE 3, COLOR_SATURATION 4,
  COLOR_X 5, COLOR_Y 6, COLOR_TEMPERATURE 7, COLOR (command) 22.
- Value kinds: NONE 0, BOOL 1, I32 2, U32 3, F32 4, ENUM 5.
- Actions: ON 1, OFF 2, TOGGLE 3, SET 4.
- Events: SINGLE_PRESS 1, DOUBLE_PRESS 2, HOLD 3.
- State key: `{entity_id u64, property_id u32, reserved u32}` (16 bytes);
  state record: `ha_value_t` (8 bytes).
- Capabilities: 216 bytes; up to 8 properties `{property, action_count,
  actions[4]}` and up to 4 event ids.
- Entity types: ENTITY 14, PROPERTY_STATE 15, CAPABILITIES 16,
  LOGICAL_DEVICE 17. They are opaque to Domain, but the numbers are canonical ABI.

## z2m-rs → Domain projection

| z2m-rs | Domain |
| --- | --- |
| `Profile` identity/fingerprints | adapter-only discovery; not in Domain |
| EUI64 + endpoint | device id + entity id seed; entity record |
| `Expose::Binary { STATE }` | capability `POWER` with ON/OFF (TOGGLE supported by V2 action map) |
| `Expose::Numeric { BRIGHTNESS 0..254 }` | capability `BRIGHTNESS` (Domain unit percent, 0..100); SET |
| `Expose::Numeric { COLOR_TEMP mireds }` | capability `COLOR_TEMPERATURE` (Domain unit Kelvin, 2000..6500); SET |
| `Expose::Color` | capability `COLOR` (SET XY) + state properties X/Y or HUE/SAT |
| `Expose::Action` | capability **events** SINGLE/DOUBLE/HOLD, not a state property |
| `Update::Property STATE` | `PROPERTY_STATE` POWER bool |
| `Update::Property BRIGHTNESS` | `PROPERTY_STATE` BRIGHTNESS float (raw 0..254 → 100/254) |
| `Update::Property COLOR_TEMP` | `PROPERTY_STATE` COLOR_TEMPERATURE float (`1e6 / mired`) |
| `Update::Property COLOR` | `PROPERTY_STATE` COLOR_X/Y (0..1) or HUE (0..360)/SAT (0..100) |
| `Update::Property COLOR_MODE` | **no canonical property**; adapter uses it to choose the projection |
| `Update::Action` | Domain event id, source entity = entity_id |
| `Profile.configure` | adapter `ConfigRun`; Domain does not see it |

Important conversions (V2 is not mireds/254):
- brightness: report `raw * 100 / 254`; command `percent * 254 / 100`, clamped.
- color temperature: report `1_000_000 / mireds`; command `1_000_000 / kelvin`,
  clamped to ZCL u16. Domain range is Kelvin.
- XY is already 0..1; HS is degrees/percent.
- V2 `COLOR` is an **XY command**; separate HUE/SAT SET commands use
  `moveToHue` / `moveToSaturation`. The current z2m profile has combined XY/HS
  commands, so individual HUE/SAT SET is not yet representable; the first
  integration should expose XY COLOR (and COLOR_TEMPERATURE) and keep combined
  HS adapters for later.

## Commands and Operation (the one V2 gap)

V2 `domain_post()` is fire-and-forget: it registers a command executor, calls it
synchronously and writes `COMMAND_SENT`; there is no OperationId or result
correlation in Domain. V3 needs the `Desired / Operation / Reported` split.

For the first integration, without changing the canonical ABI:

1. Domain command executor receives a compact canonical request
   `{entity_id, property, action, ha_command_value_t}`.
2. The adapter resolves `entity_id → (EUI64, endpoint) → Profile`, maps it to a
   z2m-rs `Command`, calls `resolve_command`/`encode`, and enqueues in
   `zigbee-exec`.
3. The adapter keeps a bounded `PendingCommand` table:
   `OperationId → {entity, property, desired value, status}`. It publishes
   `COMMAND_SENT`/`ERROR` facts.
4. `Reported` is written **only** from a device report (fromZigbee), never from
   a Default Response. A confirmed command with no report leaves
   `Desired != Reported`.
5. `Acknowledged/Confirmed/Timeout/Failed` come from `OperationResult`.

Adding a canonical `OPERATION`/`DESIRED` entity type is a later ABI decision; it
is not needed for the first read/write loop and would be inventing V2 semantics.
The adapter table keeps those fields without polluting Domain state.

## Gaps / decisions before coding

- V3 `ha-domain` is generic; the canonical V2 model (ids, layouts, derivation,
  capability/state records) is **not ported yet**. Port exact numbers/layouts,
  including SipHash derivation, into a host `ha-model` module/crate.
- V3 `ha-domain` supports RAM backing only; canonical state is fine in RAM, the
  persistent Device Registry remains the adapter's MicroDB table.
- `Profile` has no endpoint field; generated multi-endpoint profiles are skipped.
  The first integration supports single-endpoint (First = 1) and binds seeds
  with endpoint 1. Endpoint discovery/profile endpoints are a later step.
- `COLOR_MODE` has no V2 property; keep it adapter-internal.
- z2m brightness is 0..254, V2 is percent; CT is mireds vs Kelvin. These
  conversions must live in the adapter, not in Domain or z2m-rs.
- V2 `domain_post` has no result; the pending table is adapter state, bounded,
  and diagnostics are facts. Do not make Domain know OperationId.

## Minimal implementation order

1. Port the canonical V2 model (ids, records, SipHash identity) and host tests
   against the V2 vectors.
2. Adapter: binding (EUI64, endpoint) → entity/device; project `Profile.exposes`
   to capabilities; write ENTITY/LOGICAL_DEVICE/CAPABILITIES.
3. Incoming: decoded updates → PROPERTY_STATE/events with the conversions above;
   no parallel Zigbee state.
4. Outgoing: register the Domain command; pending OperationId table; z2m
   resolve/encode → `zigbee-exec`; Default Response never writes Reported.
5. Host tests with FakeTransport: desired/acked/reported, report without
   command, command without report, timeout; then one hardware pass.

Do not add WebSocket/LVGL/automation changes in this step.

## Implemented first slice (host)

- `ha-domain::canonical`: V2 ids/records and the exact SipHash-2-4 key/derivation.
  Golden: Zigbee `(0x00124B000A1B2C3D, endpoint 1)` →
  `0xdf803cbaf588876d`. Records: entity 40, device 80, state key 16,
  state 8, capability 24, capabilities 216.
- `zigbee-devices::canonical`: EUI64+endpoint → entity id, EUI64 → device id,
  `Profile.exposes` → capabilities (POWER ON/OFF, BRIGHTNESS SET,
  COLOR_TEMPERATURE SET, COLOR SET XY when the profile has the XY codec, action
  events), and state conversions:
  brightness 254-scale → percent, mireds → Kelvin, XY/HS → scalar pairs.
- Canonical command mapping: Power ON/OFF, Brightness SET percent→254,
  ColorTemp SET Kelvin→mireds, Color SET XY; `resolve_command` + `encode`
  produce cluster/command/payload. Individual HUE/SAT SET and TOGGLE remain
  explicit `None` (profiles do not expose those commands).
- Host: 123 tests total, 21 in `zigbee-devices`, 11 in `ha-domain`.

Not yet wired: Domain entity/state writes, the Command Core pending table,
OperationId routing, and the three lifecycle scenarios. Those are the next
slice; the Zigbee transport and executor are unchanged.

## Domain integration (host chain implemented)

- `ha-domain::command`: bounded `CommandCore` (8 pending), desired value and
  status; final failures free the slot, successful acknowledgements remain until
  a report satisfies them. OperationId stays outside the canonical ABI.
- `zigbee-devices::DomainAdapter`: registers LOGICAL_DEVICE/ENTITY/CAPABILITIES/
  PROPERTY_STATE tables, idempotent by deterministic ids; writes reported state
  only from decoded updates; reads it back from Domain.
- `zigbee-devices::ZigbeeSink`: canonical command → z2m command →
  `zigbee-exec` Request (Default Response expectation); OperationId returned to
  the CommandCore. `status_from_exec` maps Confirmed/Acknowledged → Acknowledged,
  Timeout → Timeout, Failed/Expired → Failed.
- Host lifecycle tests (`tests/command_lifecycle.rs`):
  1. command + AF/DefaultResponse confirmation → Operation Acknowledged, Domain
     Reported absent; later Attribute Report writes Reported=ON and clears the
     desired command;
  2. acknowledged command without report → Reported remains absent;
  3. report without command → Domain state updates, no pending command;
  4. timeout → operation Failed, slot freed, Reported unchanged.

Total host tests: 130.

Not yet wired into `ha-app`: the host chain is complete, but the app loop still
uses the old TS0041 event path; Domain registration/state routing and command
submission are the next integration step. ESP transport currently supports
On/Off commands only; Level/ColorTemp/Color need a generic ZCL send before the
hardware pass. Read Attributes results can use the same update path when routed.

## ESP boot registration (hardware)

`ha-app` registers restored devices through `DomainAdapter` after `store::init`
and registers newly identified devices from the interview queue. On P4 both
existing devices appear in Domain with deterministic canonical entity ids:

- TS0041 EUI a4c1383bf9d62a03 → entity 3fa2248091211240;
- SNZB-01P EUI 20a716fffef70d5d → entity 274002a78a9cd8bb.

Ids are identical across reboots (Domain is RAM, so it is populated again; the
identity does not change and no second entity is created for the same seed).
Button/state routing through the canonical adapter and the command submission
path are the remaining app wiring; the host lifecycle tests already cover the
semantics.

## Incoming event/state path on P4 (hardware verified)

`ha-app` decodes Attribute Reports and button ZCL commands through `z2m-rs`;
`zigbee-devices::canonical_states` produces either PROPERTY_STATE values or
canonical events. Events are published as ephemeral Domain payload facts
(EVENT), never as PROPERTY_STATE, so a reboot does not replay a press. The old
TS0041 press-count/web path is removed.

Hardware: five TS0041 presses produced exactly five
`canonical command Action(Single)` lines and five `domain: event id=1` facts,
with no property state for the press. Boot/reboot registration remains stable.

Pending: Read Attributes values to the same state path (executor results do not
yet carry cluster/attribute), CommandCore → ZigbeeSink in the app loop, and
Generic ZCL Send.

## Read Attributes context and CommandCore results in ha-app

`zigbee-exec` keeps a bounded map of completed `(OperationId, Request)` contexts
(`take_context`), so Read Attributes results can be decoded through the same
profile decoder as reports without duplicating request data in every result.
`ha-app` parses successful read payloads as an Attribute frame and pushes the
decoded updates into the same canonical state path (`ingest_read`).

The app loop also feeds every `OperationResult` into `CommandCore` via
`status_from_exec`; no command source is wired yet, so this only completes the
result half of the lifecycle. Confirmed/Acknowledged still do not write
PROPERTY_STATE.

Pending: a command submission entry point, `ZigbeeSink` usage in the app, and
Generic ZCL Send for Level/ColorTemp/Color. Host tests: 132; ESP builds.

## Generic SendZcl (host + on/off on ESP)

`zigbee-exec` now has `RequestKind::SendZcl` with cluster, command, bounded
payload, manufacturer code, frame control and expected response. `ZigbeeSink`
uses it for every canonical command, so Power/Brightness/ColorTemp/XY are
covered by FakeTransport tests (payloads verified). `ha-app` maps SendZcl OnOff
to the existing SDK call; Level/ColorTemp/Color still need the typed Espressif
ZCL command bridge (the SDK exposes per-cluster typed calls, not a raw standard
ZCL send).

The app still has no external command source: `CommandCore` receives operation
results, but nothing submits canonical commands yet. That entry point and the
typed C bridge are the remaining work.

## Typed ZCL bridge in C (ESP)

`zb_exec_send_zcl` is the transport-side dispatch for `SendZcl`: it converts the
bounded ZCL payload into the Espressif typed calls for OnOff, Level (0x0004/0x0000),
ColorTemperature (0x0300/0x0A) and XY (0x0300/0x07); unsupported cluster/command,
manufacturer-specific frames and non-cluster frame control return -1 explicitly.
Rust `SendZcl` now calls this bridge; the ESP build and P4 boot (both devices
registered) pass.

No command submission entry exists yet, and no actuator is available for a
hardware outgoing test; enhanced hue and other commands remain unsupported by
the C bridge.

## Command entry by entity id (host)

`zigbee_devices::CommandRouter` is the single command entry used by
`ha-app::submit_command`. A registration-time `Binding` carries only
`(entity, eui, short, endpoint, profile)`; callers pass entity, property and
`CommandValue` and never see EUI64 or ZCL. The router rejects unknown entities
(`NotFound`) and properties/actions absent from the profile capabilities
(`InvalidArg`) before touching the executor. Power infers ON/OFF from a boolean;
other properties use SET. Results keep the existing Desired/Operation/Reported
semantics, and the bounded CommandCore(8) returns `NoSpace` on overflow.

Host tests cover routing, power action inference, unknown entity, missing
capability and bounded overflow. No source calls `submit_command` yet; hardware
outgoing confirmation still requires an actuator.
