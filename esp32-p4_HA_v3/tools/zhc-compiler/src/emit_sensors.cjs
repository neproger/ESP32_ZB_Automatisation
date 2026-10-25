'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { pkg, devicesDir } = require('./cli.cjs');
const { packageRoot, listFiles, list } = require('./zhc.cjs');

const modelsIndex = JSON.parse(fs.readFileSync(path.join(packageRoot, 'dist/models-index.json'), 'utf8'));
const manifest = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../out/catalog_manifest.json'), 'utf8'));
const manifestBySource = new Map(manifest.definitions.map((d) => [`${d.file}#${d.index}`, d]));

function mark(target, namespace) {
  for (const key of Object.keys(target)) {
    const fn = target[key];
    if (typeof fn !== 'function') continue;
    target[key] = function (...args) {
      const extend = fn.apply(this, args);
      if (extend && typeof extend === 'object' && Object.isExtensible(extend)) {
        extend.__sensor = { helper: `${namespace}.${key}`, args: args[0] || {} };
      }
      return extend;
    };
  }
}
mark(require(path.join(packageRoot, 'dist/lib/modernExtend')), 'modernExtend');

const SUPPORTED = new Set(['modernExtend.battery', 'modernExtend.temperature', 'modernExtend.humidity', 'modernExtend.occupancy', 'modernExtend.illuminance', 'modernExtend.onOff']);

function rustString(value) {
  let out = '"';
  for (const char of String(value)) {
    const code = char.codePointAt(0);
    if (char === '"') out += '\\"';
    else if (char === '\\') out += '\\\\';
    else if (char === '\n') out += '\\n';
    else if (char === '\r') out += '\\r';
    else if (char === '\t') out += '\\t';
    else if (code < 0x20) out += `\\x${code.toString(16).padStart(2, '0')}`;
    else out += char;
  }
  return out + '"';
}

function seconds(value) {
  if (typeof value === 'number') return value;
  const map = { MIN: 1, '10_SECONDS': 10, '1_MINUTE': 60, '10_MINUTES': 600, '1_HOUR': 3600, MAX: 3600 };
  return map[value] ?? null;
}

function fingerprints(raw, file, definitionIndex, entry) {
  const found = new Map();
  const add = (manufacturer, model) => {
    if (!model) return;
    const candidates = modelsIndex[String(model).toLowerCase()] ?? [];
    if (!candidates.some(([f, i]) => f === file && i === definitionIndex)) return;
    found.set(`${manufacturer || '*'}|${model}`, { manufacturer: manufacturer || null, model });
  };
  for (const fingerprint of list(entry.fingerprint)) add(fingerprint.manufacturerName || null, fingerprint.modelID);
  for (const model of list(entry.zigbeeModel)) add(null, model);
  add(entry.manufacturerName || null, entry.model);
  return [...found.values()];
}

const profiles = [];
const skipped = { light: 0, no_fingerprint: 0, unsupported_endpoint: 0, no_sensor: 0 };

for (const file of listFiles()) {
  const module = require(path.join(devicesDir, file));
  list(module.definitions).forEach((raw, definitionIndex) => {
    const entry = manifestBySource.get(`${file}#${definitionIndex}`);
    if (!entry) return;
    const marked = list(raw.extend).filter((extend) => extend && extend.__sensor).map((extend) => extend.__sensor);
    if (marked.some((item) => item.helper === 'modernExtend.light')) {
      skipped.light += 1;
      return;
    }
    const supported = marked.filter((item) => SUPPORTED.has(item.helper));
    if (!supported.some((item) => item.helper !== 'modernExtend.onOff')) {
      skipped.no_sensor += 1;
      return;
    }
    if (supported.some((item) => item.args.endpointNames)) {
      skipped.unsupported_endpoint += 1;
      return;
    }
    const fps = fingerprints(raw, file, definitionIndex, entry);
    if (fps.length === 0) {
      skipped.no_fingerprint += 1;
      return;
    }
    const has = new Set(supported.map((item) => item.helper));
    const from = [];
    const to = [];
    const exposes = [];
    const configure = [];
    if (has.has('modernExtend.battery')) {
      from.push('BATTERY_ATTR');
      exposes.push('Expose::Numeric { property: properties::BATTERY, unit: Unit::Percent, min: 0.0, max: 100.0, step: 1.0 },');
      configure.push('ConfigureStep::Report { endpoint: ConfigureEndpoint::First, cluster: 0x0001, attribute: 0x0021, attribute_type: 0x20, min: 3600, max: 3600, change: 10 },');
      const battery = marked.find((item) => item.helper === 'modernExtend.battery').args;
      if (battery.voltage) {
        from.push('BATTERY_VOLTAGE_ATTR');
        exposes.push('Expose::Numeric { property: properties::VOLTAGE, unit: Unit::MilliVolt, min: 0.0, max: 5000.0, step: 1.0 },');
        configure.push('ConfigureStep::Report { endpoint: ConfigureEndpoint::First, cluster: 0x0001, attribute: 0x0020, attribute_type: 0x20, min: 3600, max: 3600, change: 10 },');
      }
    }
    if (has.has('modernExtend.temperature')) {
      from.push('TEMPERATURE_ATTR');
      exposes.push('Expose::Numeric { property: properties::TEMPERATURE, unit: Unit::Celsius, min: -40.0, max: 125.0, step: 0.1 },');
      configure.push('ConfigureStep::Report { endpoint: ConfigureEndpoint::First, cluster: 0x0402, attribute: 0x0000, attribute_type: 0x29, min: 60, max: 3600, change: 50 },');
    }
    if (has.has('modernExtend.humidity')) {
      from.push('HUMIDITY_ATTR');
      exposes.push('Expose::Numeric { property: properties::HUMIDITY, unit: Unit::Percent, min: 0.0, max: 100.0, step: 1.0 },');
      configure.push('ConfigureStep::Report { endpoint: ConfigureEndpoint::First, cluster: 0x0405, attribute: 0x0000, attribute_type: 0x21, min: 60, max: 3600, change: 100 },');
    }
    if (has.has('modernExtend.occupancy')) {
      from.push('OCCUPANCY_ATTR');
      exposes.push('Expose::Binary { property: properties::OCCUPANCY },');
      configure.push('ConfigureStep::Report { endpoint: ConfigureEndpoint::First, cluster: 0x0406, attribute: 0x0000, attribute_type: 0x18, min: 1, max: 3600, change: 0 },');
    }
    if (has.has('modernExtend.illuminance')) {
      from.push('ILLUMINANCE_ATTR');
      exposes.push('Expose::Numeric { property: properties::ILLUMINANCE, unit: Unit::Lux, min: 0.0, max: 100000.0, step: 1.0 },');
      configure.push('ConfigureStep::Report { endpoint: ConfigureEndpoint::First, cluster: 0x0400, attribute: 0x0000, attribute_type: 0x21, min: 60, max: 3600, change: 5 },');
    }
    if (has.has('modernExtend.onOff')) {
      from.push('ONOFF_ATTR');
      to.push('ONOFF_ON', 'ONOFF_OFF');
      exposes.push('Expose::Binary { property: properties::STATE },');
      configure.push('ConfigureStep::Bind { endpoint: ConfigureEndpoint::First, cluster: 0x0006 },');
    }
    const symbols = [];
    const fpLines = [];
    for (const [index, fp] of fps.entries()) {
      const manufacturer = fp.manufacturer ? `Manufacturer::Exact(${rustString(fp.manufacturer)})` : 'Manufacturer::Any';
      fpLines.push(`    Fingerprint { manufacturer: ${manufacturer}, model: ${rustString(fp.model)} },`);
      const symbol = `SENSOR_${profiles.length}`;
      symbols.push(symbol);
    }
    const index = profiles.length;
    const id = `zhc.sensor.${entry.id}`;
    const lines = [];
    lines.push(`static FROM_${index}: &[Converter] = &[${from.join(', ')}];`);
    lines.push(`static TO_${index}: &[CommandMap] = &[${to.join(', ')}];`);
    lines.push(`static EXP_${index}: &[Expose] = &[${exposes.join(' ')}];`);
    lines.push(`static FP_${index}: &[Fingerprint] = &[${fpLines.join('')}];`);
    lines.push(`static CFG_${index}: ConfigurePlan = ConfigurePlan { steps: &[${configure.join(' ')}], timeout_ms: 5000, retries: 2, version: 1 };`);
    lines.push(`pub static SENSOR_${index}: Profile = Profile {`);
    lines.push(`    id: ${rustString(id)}, vendor: ${rustString(entry.vendor || file)}, fingerprints: FP_${index}, exposes: EXP_${index}, from: FROM_${index}, to: TO_${index},`);
    lines.push(`    configure: Some(&CFG_${index}), omit_optional_params: false, turns_off_at_brightness_1: false,`);
    lines.push(`    support: Support::IDENTIFIED.union(Support::DECODED).union(Support::PARTIAL),`);
    lines.push('};');
    profiles.push({ id, source: { file, definition_index: definitionIndex }, symbol: `SENSOR_${index}`, lines });
  });
}

const header = [
  '// @generated by tools/zhc-compiler/src/emit_sensors.cjs from ZHC 26.123.0.',
  'use crate::capabilities::{BATTERY_ATTR, BATTERY_VOLTAGE_ATTR, HUMIDITY_ATTR, ILLUMINANCE_ATTR, OCCUPANCY_ATTR, ONOFF_ATTR, ONOFF_OFF, ONOFF_ON, TEMPERATURE_ATTR};',
  'use crate::properties;',
  'use crate::{CommandMap, ConfigureEndpoint, ConfigurePlan, ConfigureStep, Converter, Expose, Fingerprint, Manufacturer, Profile, Support, Unit};',
  '',
];
const body = profiles.flatMap((p) => [...p.lines, '']);
const allProfiles = profiles.map((_, index) => `    &SENSOR_${index},`);
fs.writeFileSync(
  path.resolve(__dirname, '../../../crates/z2m-rs/src/generated/sensors.rs'),
  [...header, ...body, 'pub static SENSORS: &[&Profile] = &[', ...allProfiles, '];', ''].join('\n'),
);
fs.mkdirSync(path.resolve(__dirname, '../out'), { recursive: true });
fs.writeFileSync(path.resolve(__dirname, '../out/sensors_manifest.json'), JSON.stringify({ profiles: profiles.map(({ lines, ...rest }) => rest), skipped }, null, 1));
console.log(`sensor profiles: ${profiles.length}`, skipped);