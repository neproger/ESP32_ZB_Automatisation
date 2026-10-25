'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { packageJson, devicesDir, listFiles, list, plain } = require('./zhc.cjs');
const { pkg } = require('./cli.cjs');

const outDir = path.join(__dirname, '..', 'out');
const baselineFile = path.join(__dirname, '..', 'baseline', 'mechanisms.json');
const updateBaseline = process.argv.includes('--update-baseline');

const manifestFile = path.join(outDir, 'catalog_manifest.json');
if (!fs.existsSync(manifestFile)) {
  console.error('run manifest first: npm run manifest');
  process.exit(2);
}
const manifest = JSON.parse(fs.readFileSync(manifestFile, 'utf8'));
const idBySource = new Map(manifest.definitions.map((entry) => [`${entry.file}#${entry.index}`, entry.id]));

const fz = require(path.join(pkg, 'dist/converters/fromZigbee'));
const tz = require(path.join(pkg, 'dist/converters/toZigbee'));
const reporting = require(path.join(pkg, 'dist/lib/reporting'));
const tuya = require(path.join(pkg, 'dist/lib/tuya'));

function markExtensions(target, namespace) {
  for (const key of Object.keys(target)) {
    const fn = target[key];
    if (typeof fn !== 'function') continue;
    target[key] = function (...args) {
      const extend = fn.apply(this, args);
      if (extend && typeof extend === 'object' && Object.isExtensible(extend)) {
        extend.__zhc_helper = `${namespace}.${key}`;
        extend.__zhc_args = args;
      }
      return extend;
    };
  }
}
markExtensions(require(path.join(pkg, 'dist/lib/modernExtend')), 'modernExtend');
markExtensions(tuya.modernExtend, 'tuya');

function reverse(map, field) {
  const out = new Map();
  for (const name of Object.keys(map).sort()) {
    const value = map[name];
    const fn = value && typeof value === 'object' ? value[field] : undefined;
    if (typeof fn === 'function' && !out.has(fn)) out.set(fn, name);
  }
  return out;
}
const FZ_NAMES = reverse(fz, 'convert');
const TZ_NAMES = reverse(tz, 'convertSet');
const REPORT_NAMES = new Map(
  Object.keys(reporting)
    .sort()
    .map((name) => [reporting[name], `reporting.${name}`])
    .filter(([fn]) => typeof fn === 'function'),
);

function sourceHash(fn) {
  try {
    return crypto.createHash('sha1').update(fn.toString()).digest('hex').slice(0, 12);
  } catch {
    return 'unavailable';
  }
}

function helperName(names, fn, prefix) {
  if (typeof fn !== 'function') return `${prefix}:missing`;
  return names.get(fn) ?? `${prefix}:anonymous:${sourceHash(fn)}`;
}

function argHash(args) {
  try {
    return crypto.createHash('sha256').update(JSON.stringify(plain(args))).digest('hex').slice(0, 16);
  } catch {
    return 'unavailable';
  }
}

function extendStatus(helper, args) {
  const options = args[0] && typeof args[0] === 'object' ? args[0] : {};
  const keys = Object.keys(options).sort();
  if (helper === 'modernExtend.onOff') {
    const bad = keys.filter((key) => !['configureReporting', 'reporting'].includes(key));
    return bad.length === 0
      ? ['supported', 'genOnOff state and reporting']
      : ['partial', `onOff options not covered: ${bad.join(',')}`];
  }
  if (helper === 'modernExtend.light') {
    return ['partial', 'standard brightness/CT/color core; non-default light options not covered'];
  }
  if (helper === 'modernExtend.battery') {
    return ['partial', 'percentage/voltage only'];
  }
  if (helper === 'modernExtend.binary') {
    return options.name === 'state' && options.valueOn === undefined && options.valueOff === undefined
      ? ['supported', 'binary state']
      : ['partial', 'non-state binary or custom values'];
  }
  if (helper === 'modernExtend.bindCluster') return ['supported', 'cluster bind'];
  if (['modernExtend.temperature', 'modernExtend.humidity', 'modernExtend.occupancy', 'modernExtend.illuminance'].includes(helper)) {
    return ['partial', 'core decode and basic reporting; non-default options not covered'];
  }
  if (['modernExtend.identify', 'modernExtend.poll', 'modernExtend.deviceEndpoints', 'modernExtend.forcePowerSource', 'modernExtend.forceDeviceType'].includes(helper)) {
    return ['partial', 'optional identify/addressing behavior not implemented'];
  }
  if (helper === 'modernExtend.commandsOnOff' || helper === 'modernExtend.commandsLevelCtrl' || helper === 'modernExtend.commandsColorCtrl') {
    return ['partial', 'command core only; optional command arguments not covered'];
  }
  if (helper.startsWith('tuya.')) return ['unsupported', 'Tuya-specific runtime'];
  return ['unsupported', 'no Rust mechanism for this modernExtend helper'];
}

function optionNames(converter) {
  return list(converter && converter.options)
    .map((option) => option && option.name)
    .filter(Boolean)
    .sort();
}

function fromStatus(helper, cluster, options) {
  if (helper === 'on_off' && cluster === 'genOnOff') {
    return options.length === 0
      ? ['supported', 'genOnOff state attribute']
      : ['partial', 'state_action option not represented in Domain'];
  }
  if (helper === 'brightness' && cluster === 'genLevelCtrl') {
    return options.length === 0
      ? ['supported', 'genLevelCtrl currentLevel']
      : ['partial', 'non-default brightness options'];
  }
  if (helper === 'color_colortemp' && cluster === 'lightingColorCtrl') {
    return ['partial', 'standard CT/XY/HS core; non-default color options not covered'];
  }
  if (helper === 'battery' && cluster === 'genPowerCfg') {
    return ['partial', 'percentage/voltage only'];
  }
  return ['unsupported', 'no Rust mechanism for this fromZigbee helper'];
}

function toStatus(helper, cluster, keys, options) {
  if (helper === 'on_off' && cluster === 'genOnOff') {
    return keys.length === 1 && keys[0] === 'state' && options.length === 0
      ? ['supported', 'genOnOff on/off']
      : ['partial', 'on/off with extra keys or options'];
  }
  if (['light_onoff_brightness', 'light_colortemp', 'light_color', 'light_hue_saturation', 'light_color_xy'].includes(helper)) {
    return ['partial', 'command core exists; optional params/keys not all covered'];
  }
  return ['unsupported', 'no Rust mechanism for this toZigbee helper'];
}

function configureStatus(fn) {
  if (typeof fn !== 'function') return ['partial', 'configure entry is not a function'];
  if (fn === tuya.configureMagicPacket) return ['supported', 'magic packet read'];
  const name = REPORT_NAMES.get(fn);
  if (name) return ['supported', name];
  let source = '';
  try {
    source = fn.toString();
  } catch {}
  if (source.includes('reporting')) return ['supported', 'reporting calls'];
  if (source.includes('.bind(')) return ['supported', 'cluster bind'];
  return ['partial', 'configure closure is not translated'];
}

function flattenExposes(value, out) {
  for (const expose of list(value)) {
    if (!expose || typeof expose !== 'object') {
      out.push({ type: typeof expose === 'function' ? 'function' : String(expose), status: 'unsupported' });
      continue;
    }
    const entry = {
      type: expose.type ?? (expose.features ? 'composite' : 'unknown'),
      property: expose.property ?? null,
      name: expose.name ?? null,
      features: list(expose.features).map((feature) => feature && feature.name).filter(Boolean),
    };
    if (entry.type === 'binary' && entry.property === 'state') out.push({ ...entry, status: 'supported', note: 'on/off state' });
    else if (entry.type === 'switch' && entry.property === 'state') out.push({ ...entry, status: 'supported', note: 'switch state' });
    else if (entry.type === 'numeric' && ['brightness', 'color_temp', 'battery', 'voltage'].includes(entry.property)) {
      out.push({ ...entry, status: entry.property === 'battery' || entry.property === 'voltage' ? 'partial' : 'supported' });
    } else if (entry.type === 'light') {
      const unsupported = entry.features.filter((feature) => !['brightness', 'color_temp', 'color_xy'].includes(feature));
      out.push({
        ...entry,
        status: unsupported.length === 0 ? 'supported' : 'partial',
        note: unsupported.length ? `features not covered: ${unsupported.join(',')}` : 'standard light features',
      });
    } else if (entry.type === 'action') {
      out.push({ ...entry, status: 'partial', note: 'button actions only where a profile exists' });
    } else {
      out.push({ ...entry, status: 'unsupported', note: 'expose type/property not represented' });
    }
  }
}

function effective(raw) {
  const items = [];
  const exposes = [];
  let onEvent = false;
  const add = (holder) => {
    for (const converter of list(holder.fromZigbee)) {
      const helper = helperName(FZ_NAMES, converter.convert, 'from');
      const options = optionNames(converter);
      const [status, note] = fromStatus(helper, converter.cluster, options);
      items.push({ kind: 'from', helper, cluster: converter.cluster ?? null, keys: [], options, status, note });
    }
    for (const converter of list(holder.toZigbee)) {
      const helper = typeof converter.convertSet === 'function'
        ? helperName(TZ_NAMES, converter.convertSet, 'to')
        : converter.convertGet
          ? 'to:read'
          : 'to:unset';
      const keys = list(converter.key).map(String).sort();
      const options = optionNames(converter);
      const [status, note] = toStatus(helper, converter.cluster, keys, options);
      items.push({ kind: 'to', helper, cluster: converter.cluster ?? null, keys, options, status, note });
    }
    for (const configure of list(holder.configure)) {
      const [status, note] = configureStatus(configure);
      items.push({ kind: 'configure', helper: note, status, note });
    }
    flattenExposes(holder.exposes, exposes);
    if (typeof holder.onEvent === 'function') onEvent = true;
  };
  add(raw);
  for (const extend of list(raw.extend)) {
    if (extend && typeof extend === 'object' && extend.__zhc_helper) {
      const args = extend.__zhc_args ?? [];
      const options = args[0] && typeof args[0] === 'object' ? Object.keys(args[0]).sort() : [];
      const [status, note] = extendStatus(extend.__zhc_helper, args);
      items.push({
        kind: 'extend',
        helper: extend.__zhc_helper,
        status,
        note,
        options,
        args_hash: argHash(args),
        contained: {
          from: list(extend.fromZigbee).length,
          to: list(extend.toZigbee).length,
          configure: list(extend.configure).length,
          exposes: list(extend.exposes).length,
          onEvent: typeof extend.onEvent === 'function',
        },
      });
    } else {
      add(extend);
    }
  }
  for (const expose of exposes) items.push({ kind: 'expose', helper: expose.type, status: expose.status, note: expose.note ?? '' });
  if (onEvent) items.push({ kind: 'onEvent', helper: 'onEvent', status: 'unsupported', note: 'no onEvent runtime' });
  return { items, exposes, onEvent };
}

function statusOf(items) {
  if (items.length === 0) return ['UNSUPPORTED', 'no behaviors or exposes'];
  const supported = items.filter((item) => item.status === 'supported').length;
  const bad = items.filter((item) => item.status !== 'supported').length;
  if (bad === 0) return ['SUPPORTED', ''];
  if (supported > 0) return ['PARTIAL', ''];
  return ['UNSUPPORTED', 'no supported mechanism'];
}

const definitions = [];
const mechanisms = new Map();
const summary = { definitions: 0, SUPPORTED: 0, PARTIAL: 0, UNSUPPORTED: 0, by_kind: {} };

for (const file of listFiles()) {
  const module = require(path.join(devicesDir, file));
  list(module.definitions).forEach((raw, index) => {
    const id = idBySource.get(`${file}#${index}`);
    if (!id) throw new Error(`definition missing from manifest: ${file}#${index}`);
    const provides = effective(raw);
    const [status, reason] = statusOf(provides.items);
    summary.definitions += 1;
    summary[status] += 1;
    for (const item of provides.items) {
      const key = `${item.kind}:${item.helper}`;
      const rank = { unsupported: 0, partial: 1, supported: 2 };
      const entry = mechanisms.get(key) ?? {
        status: item.status,
        best_status: item.status,
        note: item.note,
        count: 0,
      };
      if (rank[item.status] > rank[entry.best_status]) entry.best_status = item.status;
      if (rank[item.status] < rank[entry.status]) {
        entry.status = item.status;
        entry.note = item.note;
      }
      entry.count += 1;
      mechanisms.set(key, entry);
      const byKind = (summary.by_kind[item.kind] ??= { supported: 0, partial: 0, unsupported: 0 });
      byKind[item.status] += 1;
    }
    definitions.push({
      id,
      file,
      index,
      model: raw.model ?? null,
      vendor: raw.vendor ?? null,
      status,
      reason,
      items: provides.items,
    });
  });
}

const mechanismReport = Object.fromEntries([...mechanisms].sort(([a], [b]) => a.localeCompare(b)));
fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(
  path.join(outDir, 'coverage_manifest.json'),
  JSON.stringify({ schema: 1, package: packageJson.version, summary, mechanisms: mechanismReport, definitions }),
);

const supportedMechanisms = Object.fromEntries(
  Object.entries(mechanismReport)
    .filter(([, value]) => value.best_status === 'supported')
    .map(([key]) => [key, 'supported']),
);
const definitionStatuses = Object.fromEntries(definitions.map((definition) => [definition.id, definition.status]));
const baseline = fs.existsSync(baselineFile) ? JSON.parse(fs.readFileSync(baselineFile, 'utf8')) : null;
if (updateBaseline || !baseline) {
  fs.mkdirSync(path.dirname(baselineFile), { recursive: true });
  fs.writeFileSync(
    baselineFile,
    JSON.stringify(
      { package: packageJson.version, mechanisms: supportedMechanisms, definitions: definitionStatuses },
      null,
      1,
    ),
  );
} else {
  const regressions = [];
  for (const [key, status] of Object.entries(baseline.mechanisms)) {
    if (status === 'supported' && mechanismReport[key] && mechanismReport[key].best_status !== 'supported') {
      regressions.push(`${key}: ${status} -> ${mechanismReport[key].best_status}`);
    }
  }
  const rank = { UNSUPPORTED: 0, PARTIAL: 1, SUPPORTED: 2 };
  for (const [id, status] of Object.entries(baseline.definitions)) {
    const current = definitionStatuses[id];
    if (!current) regressions.push(`${id}: ${status} -> missing`);
    else if (rank[current] < rank[status]) regressions.push(`${id}: ${status} -> ${current}`);
  }
  if (regressions.length) {
    console.error(`coverage regression: ${regressions.length} entries lost support`);
    for (const line of regressions.slice(0, 20)) console.error(`  ${line}`);
    process.exit(1);
  }
  const newSupported = Object.keys(supportedMechanisms).filter((key) => !(key in baseline.mechanisms));
  if (newSupported.length) {
    console.warn(`new supported mechanisms (run coverage:update to accept): ${newSupported.join(', ')}`);
  }
}

for (const definition of definitions) {
  if (definition.status === 'SUPPORTED' && definition.items.some((item) => item.status !== 'supported')) {
    console.error(`false SUPPORTED: ${definition.id}`);
    process.exit(1);
  }
}
for (const [key, entry] of Object.entries(mechanismReport)) {
  if (!['supported', 'partial', 'unsupported'].includes(entry.status)) {
    console.error(`unclassified mechanism: ${key}`);
    process.exit(1);
  }
}

console.log(
  `coverage: ${summary.SUPPORTED} SUPPORTED, ${summary.PARTIAL} PARTIAL, ${summary.UNSUPPORTED} UNSUPPORTED`,
);
for (const [kind, counts] of Object.entries(summary.by_kind)) {
  console.log(`  ${kind}: ${counts.supported} supported, ${counts.partial} partial, ${counts.unsupported} unsupported`);
}
console.log(`manifest: ${path.relative(process.cwd(), path.join(outDir, 'coverage_manifest.json'))}`);
if (updateBaseline || !baseline) console.log(`baseline: ${path.relative(process.cwd(), baselineFile)}`);