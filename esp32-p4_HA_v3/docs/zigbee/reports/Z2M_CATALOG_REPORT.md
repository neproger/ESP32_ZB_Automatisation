# ZHC catalog inventory (first run)

Generated with `tools/z2m_spike/catalog.cjs` against the pinned ZHC 26.123.0:
401 device files, **4595 definitions**.

| Metric | Count |
| --- | --- |
| Definitions | 4595 |
| With any fingerprint (model or model+manufacturer) | 4595 |
| Manufacturer-specific fingerprint | **1075** |
| Model-only (`zigbeeModel`/model) | **3520** |
| With from/to/extend converters | 4586 |
| No converters | 9 |
| Fully translatable by the current table | **0** |
| Partial | 4595 |
| Unidentified | 0 |

`catalog_report.json` (regenerated, gitignored) contains every definition with
its blockers and first fingerprints.

## Top blockers

| Count | Blocker |
| --- | --- |
| 3754 | `configure:unknown` |
| 2848 | `fz:custom:genOnOff` |
| 1947 | `fz:custom:genLevelCtrl` |
| 1720 | `expose:light` |
| 1711 | `tz:power_on_behavior` |
| 1683 | `tz:level_config` |
| 1683 | `tz:brightness_move,...` |
| 1677 | `tz:brightness_step,...` |
| 1671 | `tz:rate` |
| 1253 | `fz:custom:lightingColorCtrl` |
| 1163 | `tz:color_mode` / `tz:color_options` |

## What this says

- The current compiler table covers only static attribute rules, on/off
  commands and a small configure subset. Against the real catalog that is
  **0 full definitions**; every profile would be `PARTIAL`.
- The largest single gap is **configure translation** (3754 definitions), then
  standard cluster converters (`genOnOff`, `genLevelCtrl`) and lights
  (`light` expose, `lightingColorCtrl`).
- 3520 definitions are matched only by model (`Manufacturer::Any` equivalent).
  Discovery is possible but weaker; manufacturer extraction must be preserved
  from `raw.fingerprint` where present.
- Tuya options (power-on behavior, level config, rate, color options) dominate
  the long tail; they are per-feature work, not per-model handlers.

## Next increments, in order

1. Replace the spike with a manifest-driven compiler and add the completeness
   gate (fingerprints, converters, configure, exposes; fail on silent drops).
2. Translate the standard `genOnOff`/`genLevelCtrl` converters and the
   `reporting.*` configure calls. This is the broadest coverage win.
3. Decide light support deliberately (exposes light + lightingColorCtrl) — the
   C6 lamp is a test of universality, not a reason for a special path.
4. Keep `PARTIAL` derived from the generated feature manifest, never manual.
