// Catalog inventory: every ZHC definition -> identifiable / translatable level.
// Diagnostics only; it does not emit Rust yet. Output: catalog_report.json + stdout summary.
//
// usage: node catalog.cjs <zhc-package> <dist/devices-dir>
const fs = require('node:fs');
const path = require('node:path');

const pkg = process.argv[2];
const devicesDir = process.argv[3];
if (!pkg || !devicesDir) {
  console.error('usage: node catalog.cjs <zhc-package> <dist/devices-dir>');
  process.exit(2);
}

const fz = require(path.join(pkg, 'dist/converters/fromZigbee'));
const tz = require(path.join(pkg, 'dist/converters/toZigbee'));
const tuya = require(path.join(pkg, 'dist/lib/tuya'));

// Constructs the Rust compiler can represent today (see docs/Z2M_GENERATOR_AUDIT.md).
const KNOWN_FZ = new Set([
  fz.on_off,
  fz.battery,
  fz.temperature,
  fz.humidity,
  fz.illuminance,
  fz.occupancy,
  fz.power,
  fz.voltage,
  fz.current,
  fz.energy,
]);
const KNOWN_TZ = new Set([tz.on_off]);
const EXPOSE_TYPES = new Set(['binary', 'switch', 'numeric', 'enum', 'action', 'text']);
const CONFIGURE_OK = [
  'tuya.configureMagicPacket',
  'reporting.bind',
  'reporting.onOff',
  'reporting.batteryPercentageRemaining',
  'reporting.batteryVoltage',
];
const CONFIGURE_BAD = ['endpoint.read', 'saveClusterAttributeKeyValue', 'read(', 'command(', 'write('];

const list = (value) => (Array.isArray(value) ? value : value == null ? [] : [value]);

function source(fn) {
  try {
    return typeof fn === 'function' ? fn.toString() : '';
  } catch {
    return '';
  }
}

function classifyConfigure(value) {
  const functions = [];
  if (typeof value === 'function') functions.push(value);
  else if (Array.isArray(value)) for (const item of value) if (typeof item === 'function') functions.push(item);
  if (functions.length === 0) return { ok: true, why: 'none' };
  const blockers = [];
  for (const fn of functions) {
    if (fn === tuya.configureMagicPacket) continue;
    const src = source(fn);
    if (CONFIGURE_BAD.some((needle) => src.includes(needle))) {
      blockers.push('configure:custom');
      continue;
    }
    // Dist aliases reporting as `reporting_1`; accept the standard calls.
    if (!/reporting(_\d+)?\.|\.bind\(|\.onOff\(|configureMagicPacket/.test(src)) {
      blockers.push('configure:unknown');
    }
  }
  return blockers.length === 0
    ? { ok: true, why: 'known' }
    : { ok: false, why: blockers[0] };
}

function classifyFz(converter) {
  if (!converter) return 'empty';
  if (KNOWN_FZ.has(converter.convert)) return null;
  return `fz:custom${converter.cluster ? `:${converter.cluster}` : ''}`;
}

function classifyTz(converter) {
  if (!converter) return 'empty';
  if (KNOWN_TZ.has(converter.convertSet)) return null;
  return `tz:${converter.key ? converter.key.join(',') : 'custom'}`;
}

function fingerprints(raw) {
  const out = new Set();
  for (const f of list(raw.fingerprint)) {
    if (f && f.modelID) out.add(`${f.manufacturerName || '*'}|${f.modelID}`);
  }
  for (const model of raw.zigbeeModel || []) if (model) out.add(`*|${model}`);
  if (raw.model) out.add(`${raw.manufacturerName || '*'}|${raw.model}`);
  return [...out];
}

function classify(raw) {
  const fps = fingerprints(raw);
  if (fps.length === 0) return { status: 'unidentified', blockers: ['no-fingerprint'], fps };
  const blockers = [];
  for (const item of list(raw.fromZigbee)) {
    const why = classifyFz(item);
    if (why) blockers.push(why);
  }
  for (const item of list(raw.toZigbee)) {
    const why = classifyTz(item);
    if (why) blockers.push(why);
  }
  const cfg = classifyConfigure(raw.configure);
  if (!cfg.ok) blockers.push(cfg.why);
  for (const expose of list(raw.exposes)) {
    if (expose && expose.type && !EXPOSE_TYPES.has(expose.type)) blockers.push(`expose:${expose.type}`);
    if (expose && expose.features && !expose.type) blockers.push('expose:composite');
  }
  for (const extend of list(raw.extend)) {
    for (const item of list(extend.fromZigbee)) {
      const why = classifyFz(item);
      if (why) blockers.push(why);
    }
    for (const item of list(extend.toZigbee)) {
      const why = classifyTz(item);
      if (why) blockers.push(why);
    }
    const ecfg = classifyConfigure(extend.configure);
    if (!ecfg.ok) blockers.push(ecfg.why);
    for (const expose of list(extend.exposes)) {
      if (expose && expose.type && !EXPOSE_TYPES.has(expose.type)) blockers.push(`expose:${expose.type}`);
    }
  }
  const unique = [...new Set(blockers)];
  let status = 'full';
  if (unique.length > 0) status = 'partial';
  if (list(raw.fromZigbee).length === 0 && list(raw.toZigbee).length === 0 && list(raw.extend).length === 0) {
    status = 'partial';
  }
  return { status, blockers: unique, fps };
}

const aggregate = {
  files: 0,
  definitions: 0,
  identified: 0,
  manufacturer_specific: 0,
  model_only: 0,
  with_converters: 0,
  full: 0,
  partial: 0,
  unsupported: 0,
  unidentified: 0,
  no_converters: 0,
  blockers: {},
};
const rows = [];

for (const file of fs.readdirSync(devicesDir).filter((f) => f.endsWith('.js'))) {
  let mod;
  try {
    mod = require(path.join(devicesDir, file));
  } catch (error) {
    aggregate.files += 1;
    continue;
  }
  aggregate.files += 1;
  for (const raw of list(mod.definitions)) {
    aggregate.definitions += 1;
    const result = classify(raw);
    if (result.fps.length) aggregate.identified += 1;
    if (result.fps.some((fp) => !fp.startsWith('*|'))) aggregate.manufacturer_specific += 1;
    else aggregate.model_only += 1;
    if (
      list(raw.fromZigbee).length +
        list(raw.toZigbee).length +
        list(raw.extend).length >
      0
    ) {
      aggregate.with_converters += 1;
    }
    aggregate[result.status] += 1;
    if (
      list(raw.fromZigbee).length === 0 &&
      list(raw.toZigbee).length === 0 &&
      list(raw.extend).length === 0
    ) {
      aggregate.no_converters += 1;
    }
    for (const blocker of result.blockers) {
      aggregate.blockers[blocker] = (aggregate.blockers[blocker] || 0) + 1;
    }
    rows.push({
      file,
      model: raw.model || (raw.zigbeeModel || [])[0] || '',
      vendor: raw.vendor || '',
      status: result.status,
      blockers: result.blockers,
      fingerprints: result.fps.slice(0, 4),
    });
  }
}

const top = Object.entries(aggregate.blockers)
  .sort((a, b) => b[1] - a[1])
  .slice(0, 25)
  .map(([reason, count]) => ({ reason, count }));

const report = { generated_from: pkg, aggregate: { ...aggregate, top_blockers: top }, definitions: rows };
fs.writeFileSync(path.join(__dirname, 'catalog_report.json'), JSON.stringify(report, null, 1));

console.log(`files: ${aggregate.files}, definitions: ${aggregate.definitions}`);
console.log(
  `identified: ${aggregate.identified} (manufacturer: ${aggregate.manufacturer_specific}, ` +
    `model-only: ${aggregate.model_only}), with converters: ${aggregate.with_converters}`,
);
console.log(
  `full: ${aggregate.full}, partial: ${aggregate.partial}, ` +
    `unsupported: ${aggregate.unsupported}, unidentified: ${aggregate.unidentified}`,
);
console.log('top blockers:');
for (const row of top) console.log(`  ${row.count}\t${row.reason}`);
