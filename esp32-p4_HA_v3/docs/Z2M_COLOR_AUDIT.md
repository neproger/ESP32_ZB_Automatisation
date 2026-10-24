# ZHC ColorControl audit (26.123.0)

Source: `dist/converters/fromZigbee.js` (`color_colortemp`),
`dist/converters/toZigbee.js` (`light_color`, `light_colortemp`),
`dist/lib/modernExtend.js` (`light`).

## Incoming (`fz.color_colortemp`)

| ZCL input | Z2M property | Conversion |
| --- | --- | --- |
| `lightingColorCtrl/colorTemperature` (0x0007) | `color_temp` | raw mireds |
| `startUpColorTemperature` | `color_temp_startup` | raw mireds |
| `colorMode` (0x0008) | `color_mode` | lookup name |
| `currentX` (0x0003) | `color.x` | 0..65535 -> 0..1, 4 decimals |
| `currentY` (0x0004) | `color.y` | 0..65535 -> 0..1, 4 decimals |
| `currentHue` (0x0000) | `color.hue` | 0..254 -> 0..360, 0 decimals |
| `enhancedCurrentHue` | `color.hue` | 0..65535 -> 0..360, 1 decimal |
| `currentSaturation` (0x0001) | `color.saturation` | 0..254 -> 0..100 |

X/Y and hue/saturation arrive as separate attributes; the composite `color`
must be assembled from the same frame. `color_mode` is a separate property.

## Outgoing (`tz.light_color`)

The converter selects a mode from the value and `meta.supportsHueAndSaturation`
(not from the mere presence of cluster 0x0300):

- **HS** (supportsHueAndSaturation):
  - `color_mode = hs`;
  - `moveToHueAndSaturation` (ZCL 0x06), cluster `lightingColorCtrl`
    (0x0300): hue 0..360 -> 0..254, saturation 0..100 -> 0..254,
    transition u16, `optionsMask/optionsOverride` unless
    `omitOptionalLevelAndColorParams`.
  - Enhanced hue uses `enhancedMoveToHueAndSaturation` and 0..65535.
  - A `value` (brightness) in the HS object is sent as
    `moveToLevelWithOnOff` (level 0..254).
- **XY** (default in `modernExtend.light` when `color` is enabled):
  - `moveToColor` (ZCL 0x07): x/y 0..1 -> 0..65535, transition u16,
    options mask/override unless omitted.
- Hue-only / saturation-only use `moveToHue`, `moveToSaturation`,
  `moveHue`, etc.; these are separate operations, not one generic color write.
- `tz.light_colortemp`: `moveToColorTemp` (0x0A), mireds u16, transition u16,
  same options. `color_temp_startup` is a separate write/command.

## `modernExtend.light` parameters that affect color

- `color: {modes, applyRedFix, enhancedHue}` (default modes `["xy"]`);
  `meta.supportsHueAndSaturation`, `supportsEnhancedHue`, `applyRedFix`.
- `colorTemp: {range, startup}`; expose `value_min/value_max` in mireds.
- `configureReporting` (default false).
- `lightingColorCtrl` colour capabilities and physical min/max are **read** by
  `light.configure`; this is not a bind/report by itself.
- `optionsMask`/`optionsOverride` and transitions are part of the payload, not
  profile-global flags.

## Rust slice required

1. A color value (`XY` and `HS`) and a color expose; scalar `Value` cannot carry
   both coordinates.
2. `CommandCodec::ColorXy` (0x07) and `ColorHs` (0x06), with mode selection from
   the profile (the generator emits the modes actually enabled), transition and
   options fields.
3. Frame decoder for `color_colortemp`: assemble x/y or h/s from one attribute
   frame, apply the exact Z2M mappings above, emit `color` and `color_mode`.
4. Generator: emit color profiles only for the modes in the original
   `modernExtend.light` args; keep manual `fromZigbee`/`toZigbee`/`configure`/
   `onEvent` in the completeness calculation (their presence must cap support).
5. Comparative golden: run the original JS converters on fixed values and
   compare the Rust command/payload and decoded properties. Cover XY, HS,
   enhanced hue, color temp, mode switching and omitted options.

## Pitfalls confirmed by the sources

- Do not choose HS merely because cluster 0x0300 exists; XY is the
  `modernExtend.light` default, HS requires explicit modes and metadata.
- `moveToHueAndSaturation` is 0x06, `moveToColor` is 0x07, `moveToColorTemp`
  is 0x0A; transition and options bytes are part of the payload.
- X/Y precision is 4 decimals; integer 0..100 mapping is not equivalent.
- `turnsOffAtBrightness1`, `levelConfig`, `effect`, `powerOnBehavior` and manual
  converters are separate behavior; profiles must stay `PARTIAL` until ported.

## Implemented slice

- `Value::Color` with `ColorValue::Xy { x: Option, y: Option }` and
  `ColorValue::Hs { hue: Option, saturation: Option }`; partial frames keep the
  missing coordinate missing.
- `COLOR_FRAME` decodes the whole `lightingColorCtrl` frame (currentX/Y,
  currentHue/Saturation, enhancedCurrentHue, colorMode, colorTemperature) with
  the Z2M mappings above.
- `CommandCodec::ColorXy` (0x07), `ColorHs` (0x06), `ColorHsEnhanced` (0x43);
  transition 0 and options mask/override are emitted; XY uses 0..65535 and HS
  0..254. Command selection matches the color representation, so a profile can
  carry both and the caller's value picks the mode.
- Generator now emits **1621 profiles**: 481 dim, 470 dim+CT, 670 with color
  (XY/HS from the original `color.modes`). 44 multi-endpoint and 55 missing-args
  definitions are skipped; `configureReporting`, CT ranges and
  `moveToLevelWithOnOffDisable` are carried.
- Host tests: color frame (full/partial/HS), XY/HS/enhanced payloads, generated
  color decode/encode. ESP app compiles; text 2,115,071 B.

Still pending: a table generated by executing the original ZHC converters and
comparing it to Rust (the current tests encode the audit's formulas and the
generator parameters, but do not call JS at test time). Options remain
`PARTIAL`.
