// Emit real lighting profiles from ZHC modernExtend descriptors, carrying the
// original parameters (endpoint constraints, brightness/CT ranges, command and
// reporting choices). Unsupported options are recorded, never defaulted.
//
// usage: node emit_lighting.cjs [zhc-package] [dist/devices-dir]
// Defaults come from tools/zhc-compiler's pinned npm dependency.
const fs = require('node:fs');
const path = require('node:path');
const { pkg, devicesDir } = require('../zhc-compiler/src/cli.cjs');
const list = (value) => (Array.isArray(value) ? value : value == null ? [] : [value]);

// Wrap light() before any device module loads, so args are associated with the
// exact extend object a definition uses.
const lightArgs = new Map();
function wrapLight(owner) {
  if (!owner || typeof owner.light !== 'function') return;
  const original = owner.light;
  owner.light = function (...args) {
    const result = original.apply(this, args);
    lightArgs.set(result, args[0] || {});
    return result;
  };
}
const modern = require(path.join(pkg, 'dist/lib/modernExtend'));
wrapLight(modern);
const tuya = require(path.join(pkg, 'dist/lib/tuya'));
if (tuya.modernExtend && tuya.modernExtend.light !== modern.light) wrapLight(tuya.modernExtend);

function lightExpose(raw) {
  for (const item of [...list(raw.exposes), ...list(raw.extend).flatMap((e) => list(e.exposes))]) {
    if (item && item.type === 'light') return item;
  }
  return null;
}

function lightExtend(raw) {
  return list(raw.extend).find((e) => list(e.exposes).some((x) => x && x.type === 'light'));
}

function feature(light, name) {
  return list(light.features).find((item) => item && item.name === name);
}

function escape(text) {
  return String(text).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

function slug(text) {
  return String(text).replace(/[^A-Za-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'device';
}

function fingerprints(raw) {
  const out = new Map();
  const add = (manufacturer, model) => {
    if (!model) return;
    const key = `${manufacturer || '*'}|${model}`;
    if (!out.has(key)) out.set(key, { manufacturer, model });
  };
  for (const fp of list(raw.fingerprint)) {
    if (fp && fp.modelID) add(fp.manufacturerName || null, fp.modelID);
  }
  for (const model of list(raw.zigbeeModel)) add(null, model);
  add(raw.manufacturerName || null, raw.model);
  return [...out.values()];
}

const profiles = [];
const skipped = {
  unsupported_color: 0,
  multi_endpoint: 0,
  no_light: 0,
  no_light_args: 0,
  no_brightness: 0,
  no_range: 0,
  no_fingerprint: 0,
};
const options = {};
function noteOption(name) {
  options[name] = (options[name] || 0) + 1;
}

for (const file of fs.readdirSync(devicesDir).filter((f) => f.endsWith('.js'))) {
  let mod;
  try {
    mod = require(path.join(devicesDir, file));
  } catch {
    continue;
  }
  let index = 0;
  for (const raw of list(mod.definitions)) {
    const light = lightExpose(raw);
    if (!light) {
      skipped.no_light += 1;
      continue;
    }
    const extend = lightExtend(raw);
    const args = extend ? lightArgs.get(extend) : undefined;
    if (!args) {
      skipped.no_light_args += 1;
      continue;
    }
    const names = new Set(list(light.features).map((f) => f && f.name).filter(Boolean));
    const colorArg = args.color;
    const colorModes = colorArg
      ? typeof colorArg === 'object' && Array.isArray(colorArg.modes)
        ? colorArg.modes
        : ['xy']
      : [];
    if (colorModes.some((mode) => mode !== 'xy' && mode !== 'hs')) {
      skipped.unsupported_color += 1;
      continue;
    }
    if (['color', 'color_hs', 'color_xy'].some((name) => names.has(name)) && colorModes.length === 0) {
      skipped.unsupported_color += 1;
      continue;
    }
    const hasColor = colorModes.length > 0;
    const enhanced = hasColor && typeof colorArg === 'object' && colorArg.enhancedHue === true;
    if (args.endpointNames || (raw.meta && raw.meta.multiEndpoint)) {
      skipped.multi_endpoint += 1;
      continue;
    }
    const brightness = feature(light, 'brightness');
    if (!brightness) {
      skipped.no_brightness += 1;
      continue;
    }
    const ct = feature(light, 'color_temp');
    const ctRange = ct ? [ct.value_min, ct.value_max] : args.colorTemp && args.colorTemp.range;
    if (ct && (!Number.isFinite(ct.value_min) || !Number.isFinite(ct.value_max))) {
      skipped.no_range += 1;
      continue;
    }
    if (ct && (!Array.isArray(ctRange) || !ctRange.every(Number.isFinite))) {
      skipped.no_range += 1;
      continue;
    }
    const fps = fingerprints(raw);
    if (fps.length === 0) {
      skipped.no_fingerprint += 1;
      continue;
    }
    const bmin = Number.isFinite(brightness.value_min) ? brightness.value_min : 0;
    const bmax = Number.isFinite(brightness.value_max) ? brightness.value_max : 254;
    const configureReporting = args.configureReporting === true;
    const levelNoOnOff = args.moveToLevelWithOnOffDisable === true;
    const omitOptions = !!(raw.meta && raw.meta.omitOptionalLevelAndColorParams === true);
    const turnsOff1 = args.turnsOffAtBrightness1 === true;
    for (const option of [
      args.levelConfig && 'level_config',
      args.effect !== false && args.effect !== undefined && 'effect',
      args.powerOnBehavior !== false && args.powerOnBehavior !== undefined && 'power_on_behavior',
      args.turnsOffAtBrightness1 && 'turns_off_at_brightness_1',
      args.configureReporting && 'configure_reporting',
      args.moveToLevelWithOnOffDisable && 'move_to_level_without_onoff',
      omitOptions && 'omit_optional_params',
    ]) {
      if (option) noteOption(option);
    }
    profiles.push({
      id: `zhc.${slug(path.basename(file, '.js'))}.${index}`,
      vendor: raw.vendor || path.basename(file, '.js'),
      tier: ct ? (hasColor ? 'color_ct' : 'dim_ct') : hasColor ? 'color' : 'dim',
      fingerprints: fps,
      bmin,
      bmax,
      ctRange: ct ? [Number(ct.value_min), Number(ct.value_max)] : null,
      configureReporting,
      levelNoOnOff,
      omitOptions,
      turnsOff1,
      hasColor,
      colorModes,
      enhanced,
    });
    index += 1;
  }
}

const lines = [];
lines.push('// Generated by tools/z2m_spike/emit_lighting.cjs. Do not edit.');
lines.push('use crate::capabilities::{');
lines.push(
  '    COLOR_ENHANCED_TO, COLOR_FRAME, COLOR_HS_TO, COLOR_XY_TO,',
);
lines.push(
  '    COLORTEMP_TO, DIM_CONFIGURE, DIM_CT_CONFIGURE, DIM_CT_FROM, DIM_CT_READ,',
);
lines.push('    DIM_FROM, LEVEL_ATTR, LEVEL_TO, LEVEL_TO_NO_ONOFF, ONOFF_ATTR, ONOFF_OFF,');
lines.push('    ONOFF_ON,');
lines.push('};');
lines.push('use crate::properties::{BRIGHTNESS, COLOR, COLOR_TEMP, STATE};');
lines.push('use crate::{CommandMap, Converter, Expose, Fingerprint, Manufacturer, Profile, Support, Unit};');
lines.push('');
for (const [index, profile] of profiles.entries()) {
  lines.push(`static FP_${index}: &[Fingerprint] = &[`);
  for (const fp of profile.fingerprints) {
    const manufacturer = fp.manufacturer
      ? `Manufacturer::Exact("${escape(fp.manufacturer)}")`
      : 'Manufacturer::Any';
    lines.push(`    Fingerprint { manufacturer: ${manufacturer}, model: "${escape(fp.model)}" },`);
  }
  lines.push('];');
  lines.push(`static EXP_${index}: &[Expose] = &[`);
  lines.push('    Expose::Binary { property: STATE },');
  lines.push('    Expose::Numeric {');
  lines.push('        property: BRIGHTNESS,');
  lines.push('        unit: Unit::None,');
  lines.push(`        min: ${profile.bmin}.0,`);
  lines.push(`        max: ${profile.bmax}.0,`);
  lines.push('        step: 1.0,');
  lines.push('    },');
  if (profile.ctRange) {
    lines.push('    Expose::Numeric {');
    lines.push('        property: COLOR_TEMP,');
    lines.push('        unit: Unit::Mired,');
    lines.push(`        min: ${profile.ctRange[0]}.0,`);
    lines.push(`        max: ${profile.ctRange[1]}.0,`);
    lines.push('        step: 1.0,');
    lines.push('    },');
  }
  if (profile.hasColor) lines.push('    Expose::Color { property: COLOR },');
  lines.push('];');
  const level = profile.levelNoOnOff ? 'LEVEL_TO_NO_ONOFF' : 'LEVEL_TO';
  lines.push(`static TO_${index}: &[CommandMap] = &[`);
  lines.push('    ONOFF_ON,');
  lines.push('    ONOFF_OFF,');
  lines.push(`    ${level},`);
  if (profile.ctRange) lines.push('    COLORTEMP_TO,');
  if (profile.hasColor && profile.colorModes.includes('xy')) lines.push('    COLOR_XY_TO,');
  if (profile.hasColor && profile.colorModes.includes('hs')) {
    lines.push(`    ${profile.enhanced ? 'COLOR_ENHANCED_TO' : 'COLOR_HS_TO'},`);
  }
  lines.push('];');
  let from = profile.ctRange ? 'DIM_CT_FROM' : 'DIM_FROM';
  if (profile.hasColor) {
    lines.push(`static FROM_${index}: &[Converter] = &[`);
    lines.push('    ONOFF_ATTR, LEVEL_ATTR,');
    lines.push('    COLOR_FRAME,');
    lines.push('];');
    from = `FROM_${index}`;
  }
  lines.push(`pub static ZHCL_${index}: Profile = Profile {`);
  lines.push(`    id: "${escape(profile.id)}",`);
  lines.push(`    vendor: "${escape(profile.vendor)}",`);
  lines.push(`    fingerprints: FP_${index},`);
  lines.push(`    exposes: EXP_${index},`);
  lines.push(`    from: ${from},`);
  lines.push(`    to: TO_${index},`);
  const configure = profile.configureReporting
    ? profile.ctRange
      ? 'Some(&DIM_CT_CONFIGURE)'
      : 'Some(&DIM_CONFIGURE)'
    : profile.ctRange || profile.hasColor
      ? 'Some(&DIM_CT_READ)'
      : 'None';
  lines.push(`    configure: ${configure},`);
  lines.push(`    omit_optional_params: ${profile.omitOptions},`);
  lines.push(`    turns_off_at_brightness_1: ${profile.turnsOff1},`);
  lines.push(
    '    support: Support::IDENTIFIED.union(Support::DECODED).union(Support::CONTROLLED).union(Support::PARTIAL),',
  );
  lines.push('};');
}
lines.push('');
lines.push('pub static LIGHTING: &[&Profile] = &[');
for (let index = 0; index < profiles.length; index += 1) lines.push(`    &ZHCL_${index},`);
lines.push('];');

const out = path.resolve(__dirname, '../../crates/z2m-rs/src/generated/lighting.rs');
fs.writeFileSync(out, lines.join('\n'));
fs.writeFileSync(
  path.join(__dirname, 'lighting_manifest.json'),
  JSON.stringify({ profiles, skipped, options }, null, 1),
);
console.log(
  JSON.stringify(
    {
      profiles: profiles.length,
      dim: profiles.filter((p) => p.tier === 'dim').length,
      dim_ct: profiles.filter((p) => p.tier === 'dim_ct').length,
      configureReporting: profiles.filter((p) => p.configureReporting).length,
      levelNoOnOff: profiles.filter((p) => p.levelNoOnOff).length,
      omitOptions: profiles.filter((p) => p.omitOptions).length,
      turnsOff1: profiles.filter((p) => p.turnsOff1).length,
      skipped,
      options,
      bytes: fs.statSync(out).size,
    },
    null,
    2,
  ),
);
