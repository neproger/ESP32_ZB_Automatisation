# Z2M generator audit

The two JS tools are **spikes**, not a catalog compiler:

- `tools/z2m_spike/generate.cjs` records `modernExtend`/`tuya` call provenance
  for three hardcoded targets and writes `report.json`. It does not emit Rust.
- `tools/z2m_spike/compile.cjs` is hardcoded to `SNZB-01P`; it emits
  `generated/snzb_01p.rs` and its golden TSV. It has no model loop and no
  fingerprint/configure translation.
- `generated/ts011f.rs` and the `TUYA_TS0041` profile are hand-ported fragments,
  not generator output (despite the `@generated` header).

## What the audit explains

| Question | Fact |
| --- | --- |
| Why were `_TZ3008_`/`_TZ3210_` missing? | Fingerprints are never extracted. `compile.cjs` writes a fixed `Manufacturer::Any` for SNZB; TS011F was hand-written with one `_TZ3000_` prefix. |
| Why was the TS011F ConfigurePlan missing? | `configure` is not compiled. `generate.cjs` only classifies it as `fragments`; `compile.cjs` maps only the SNZB battery reporting call. |
| Which modernExtend are supported? | Only the SNZB battery fragment (`percentage/voltage/reporting`). Everything else is recorded as provenance, not translated. |
| Why did the spike pick the wrong TS011F? | `generate.cjs` takes the first model match; for TS011F that was `zemismart.js` (ZMO-606-20A), not the Tuya `TS011F_plug_1` definition. |
| Can incompleteness be detected? | `report.json` stores `states`, `counts` and `provenance`, but no test or build step fails on `configure: fragments`, zero exposes or missing fingerprints. |

## Required mechanism (not another profile patch)

1. A model manifest: `(vendor, model, manufacturer)` -> source definition. Never
   "first file that contains the model"; disambiguate by fingerprint/priority.
2. Fingerprints from the selected definition: every `raw.fingerprint` entry
   (`modelID` + `manufacturerName`), `zigbeeModel`, `whiteLabel`, preserving
   exact/prefix matching.
3. A real translate table for supported helpers:
   - `tuya.configureMagicPacket` -> magic Read step;
   - `reporting.bind/onOff/...` -> Bind/Report steps;
   - attribute `fromZigbee` rules and standard `on_off` commands -> converters;
   - everything else -> an explicit unsupported entry (never silently dropped).
4. Generated completeness manifest per profile: covered exposes/converters/
   configure helpers, and unsupported ones. `PARTIAL` is derived from it.
5. A host/CI check over the manifest: a profile marked `CONTROLLED` must have a
   command map; a configure helper must map to a step; fingerprints must cover
   every manufacturer listed by the selected ZHC definition. Missing coverage
   fails the check instead of appearing only when a device is paired.
6. Regenerate the three existing profiles from the mechanism, delete the
   hand-written fragments, and compare their goldens.

## Order of work

The Command API and the generator are independent. The Command API can be built
and host-tested against the current three profiles (`resolve_command` ->
`RequestKind::Zcl` -> `zigbee-exec` fake), including the Desired / Operation /
Reported split. The generator work above is a separate, larger task and should
land before adding new device models. No JS runtime on the ESP32: keep the
build-time conversion.

## Coverage manifest and gate (implemented)

`tools/zhc-compiler/src/coverage.cjs` reads the pinned ZHC install and the
catalog manifest, then records for every definition: exposures, from/to
converters, configure entries, `onEvent`, `modernExtend` helpers with their
option keys, and the translated status of each item. Definitions are classified
as `SUPPORTED` (every item supported), `PARTIAL` (at least one supported item,
some missing) or `UNSUPPORTED` (no supported item).

The gate fails only on real losses: a supported mechanism or definition status
dropping against `baseline/mechanisms.json`, a false `SUPPORTED` definition,
or an unclassified item. `PARTIAL` is allowed. The baseline is updated
explicitly with `npm run coverage:update`.

Current snapshot: 76 SUPPORTED, 1143 PARTIAL, 3376 UNSUPPORTED. The largest
unsupported groups are non-core helpers (`modernExtend.numeric`, temperature,
humidity, electricity, Tuya-specific behavior) and configure closures that are
not translated. This is a coverage statement, not a model-by-model port.

Node >=22.12 is required in `package.json`; Node 20 prints a warning but can
still run local analysis. Fingerprint parity between the Node selector and
`z2m-rs::identify()` is not yet gated.

## Identification differential (implemented)

`tools/zhc-compiler/src/identify.cjs` now executes the original ZHC
`findDefinition` for fingerprints taken from the generated lighting profiles and
writes `crates/z2m-rs/tests/golden/identify.tsv`. `golden_identify.rs` feeds the
same model/manufacturer to `z2m-rs::identify()` and compares the selected
profile id. The current golden has 3730 cases and passes.

The matcher was corrected as a common mechanism, not per model: model IDs are
normalized (NUL tail, trim), manufacturer-specific fingerprints outrank
prefix/Any, trailing NUL/space is normalized, and model-only fallback is
restricted to `Any` fingerprints.

Not yet gated: duplicate fingerprints (collisions), profiles whose source
definition ZHC does not select for that fingerprint (`shadowed`), and
fingerprints not present in the ZHC index (`missing source`). They are counted
in `out/identify_report.json` and excluded from the golden. Endpoints, regex
fingerprints, priority fields and whiteLabel alias selection still need the
full interview/matcher stage.

## Complex matcher diagnostics (current)

`tools/zhc-compiler/src/complex.cjs` feeds ZHC `findDefinition` the full fields of
complex fingerprints and records the selected source definition. Current run:

- endpoints: 68 cases, all matched;
- regex (simple `ieeeAddr` patterns): 21 cases, all matched;
- priority: 14 cases, all matched;
- whiteLabel: 252 cases, 241 matched, 11 missing from the model index.

The remaining 15 duplicate-fingerprint collisions and 33 shadowed profiles are
reported by `identify.cjs`; they are not yet represented by the profile-list
matcher. The missing whiteLabel aliases are a source/index coverage gap, not a
matcher decision.

## Matcher index (implemented)

`tools/zhc-compiler/src/matcher.cjs` generates
`crates/z2m-rs/src/generated/matcher_data.rs` from `models-index.json`:
candidate order, full fingerprint rules (model/manufacturer/type/power source,
versions, regex `ieeeAddr`, endpoints/clusters, priority), zigbeeModel fallback
and whiteLabel metadata. Entries reference existing `Profile` values; no
converters are copied. `z2m-rs::identify()` now selects through
`match_definition()`, which mirrors ZHC `findDefinition()` including the
single-candidate shortcut, highest-priority fingerprint and model fallback.

`complex.cjs` emits `matcher_complex_cases.rs` with the full device features;
the Rust golden checks 2899 cases (all ZHC fingerprints with a resolved source,
plus whiteLabel aliases). The earlier 3728 model/manufacturer cases remain in
`golden_identify.rs`. Interview completeness (e.g. Simple Descriptor not yet
collected) is separate from the matcher; missing fields simply fail the same
fingerprint rule ZHC would fail and never produce a fabricated match.
