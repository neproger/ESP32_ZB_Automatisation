# Standard lighting family inventory

Generated with `tools/z2m_spike/lighting.cjs` (ZHC 26.123.0, 4595 definitions).

## Size

| Tier | Count |
| --- | --- |
| Definitions with a `light` expose | **1720** |
| On/off only | 0 |
| Dimmable (brightness, no color) | 555 |
| Dimmable + color temperature | **476** |
| Color (any color mode) | **689** |

A first standard template (`state`, `brightness`, `color_temp`) targets **1031
definitions**; color adds **689** and is a separate capability step.

## Why `template_candidates` is 0

The inventory intentionally checks JS function identity (`fz.on_off`,
`fz.brightness`, `fz.color_colortemp`, `tz.on_off`, ...). Lights built by
`modernExtend.light` use semantically identical but wrapped/custom converter
functions, so identity matching rejects them. This is a **classifier
limitation**, not evidence that the family is unsupported. The compiler must map
by cluster/attribute/command semantics, then apply parameters.

The same applies to configure: 1711 lights report `configure` as unknown because
the closure text does not match the narrow `reporting.*` regex. Configure must
be evaluated through the modernExtend descriptors, not guessed from source text.

## What blocks a "full" lighting profile

Most lights carry options beyond state/brightness/color_temp:

| Count | Option / blocker |
| --- | --- |
| 1683 | `level_config` |
| 1680 | `brightness_move` / `brightness_move_onoff` |
| 1675 | `brightness_step` / `brightness_step_onoff` |
| 1671 | `rate` |
| 1346 | `power_on_behavior` |
| 1161 | `color_mode` / `color_options` |
| 1131 | `color_temp_move` / `color_temp_step` |
| 993 | `color_temp_startup` |
| 975 | `effect` / `alert` / `flash` |
| 711 | `identify` |

So a minimal `state+brightness+color_temp` template is **not** a full Z2M light:
most devices would be `PARTIAL` until these options are parameterised. That is
the honest baseline.

## Contract (compiled profile <-> Rust handlers)

`Profile` already is the compiled definition: identity/fingerprints, exposes,
converters, configure, support. The missing pieces for the family:

1. **Dynamic command payloads.** `CommandMap { command, cluster, zcl_command,
   args }` can only encode fixed bytes. Lighting needs
   `encode(value) -> payload`:
   - `Level` (brightness 0..100 -> level 0..254 + transition),
   - `ColorTemp` (mireds + transition),
   - `Color` (HS/XY + transition).
   Introduce `CommandCodec` and a range command descriptor; `resolve_command`
   stays as is, `encode` dispatches to the codec.
2. **Converter descriptors** are already Rust enums (`AttributeRule`,
   `Command`, `Codec`): the generator must emit these plus parameters, not JS.
3. **Configure** stays `ConfigurePlan` (Bind/Report/Read); the generator emits
   it from `modernExtend.light` descriptors, including options it can express.
4. **Unsupported manifest** per profile: every option/helper not emitted is
   listed; `PARTIAL` is derived, never manual.

## Next implementation slice

1. Add `CommandCodec` + dynamic `encode` (Level, ColorTemp) and host tests.
2. Build a generator that maps `modernExtend.light` by descriptors (not source
   identity) into a `LIGHT_STANDARD` profile with fingerprints for the
   dimmable/CT family; emit an unsupported manifest (color, options).
3. Golden-test decode/encode for on/off, brightness, color temperature.
4. Only then add color and the option handlers, measured by the manifest.
