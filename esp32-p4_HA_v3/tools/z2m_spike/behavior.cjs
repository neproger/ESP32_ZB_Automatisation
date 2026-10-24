// Behavior inventory: shared vs per-device implementations in the ZHC catalog.
// Counts unique functions by source hash instead of counting definitions.
//
// usage: node behavior.cjs <zhc-package> <dist/devices-dir>
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const pkg = process.argv[2];
const devicesDir = process.argv[3];
if (!pkg || !devicesDir) {
  console.error('usage: node behavior.cjs <zhc-package> <dist/devices-dir>');
  process.exit(2);
}

const fz = require(path.join(pkg, 'dist/converters/fromZigbee'));
const tz = require(path.join(pkg, 'dist/converters/toZigbee'));
const list = (value) => (Array.isArray(value) ? value : value == null ? [] : [value]);

const names = new Map();
function remember(prefix, module) {
  for (const [name, converter] of Object.entries(module)) {
    if (!converter || typeof converter !== 'object') continue;
    for (const field of ['convert', 'convertSet', 'convertGet']) {
      if (typeof converter[field] === 'function') {
        names.set(converter[field], `${prefix}.${name}`);
      }
    }
  }
}
remember('fz', fz);
remember('tz', tz);

const hashes = new Map(); // hash -> { source, name, count, converters, configures, events }

function hashOf(fn) {
  const source = typeof fn === 'function' ? fn.toString() : '';
  if (!source) return null;
  return crypto.createHash('sha1').update(source).digest('hex');
}

function classifyFunction(fn, kind, cluster) {
  const hash = hashOf(fn);
  if (!hash) return null;
  let entry = hashes.get(hash);
  if (!entry) {
    entry = {
      hash,
      name: names.get(fn) || fn.name || 'inline',
      source_len: typeof fn === 'function' ? fn.toString().length : 0,
      count: 0,
      converters: 0,
      configures: 0,
      events: 0,
      clusters: new Map(),
    };
    hashes.set(hash, entry);
  }
  entry.count += 1;
  if (kind === 'configure') entry.configures += 1;
  else if (kind === 'onEvent') entry.events += 1;
  else entry.converters += 1;
  if (cluster) entry.clusters.set(String(cluster), (entry.clusters.get(String(cluster)) || 0) + 1);
  return entry;
}

function collectFrom(items, kind) {
  const out = [];
  for (const item of list(items)) {
    if (!item) continue;
    const fn = kind === 'to' ? item.convertSet || item.convertGet : item.convert;
    const entry = classifyFunction(fn, 'converter', item.cluster);
    if (entry) out.push(entry);
  }
  return out;
}

function collectFunctions(value, kind) {
  const out = [];
  for (const item of list(value)) {
    if (typeof item !== 'function') continue;
    const entry = classifyFunction(item, kind);
    if (entry) out.push(entry);
  }
  return out;
}

const definitions = [];
let fileCount = 0;
for (const file of fs.readdirSync(devicesDir).filter((f) => f.endsWith('.js'))) {
  let mod;
  try {
    mod = require(path.join(devicesDir, file));
  } catch {
    fileCount += 1;
    continue;
  }
  fileCount += 1;
  for (const raw of list(mod.definitions)) {
    const converters = [...collectFrom(raw.fromZigbee, 'from'), ...collectFrom(raw.toZigbee, 'to')];
    const configures = collectFunctions(raw.configure, 'configure');
    const events = collectFunctions(raw.onEvent, 'onEvent');
    for (const extend of list(raw.extend)) {
      converters.push(...collectFrom(extend.fromZigbee, 'from'));
      converters.push(...collectFrom(extend.toZigbee, 'to'));
      configures.push(...collectFunctions(extend.configure, 'configure'));
      events.push(...collectFunctions(extend.onEvent, 'onEvent'));
    }
    const all = [...converters, ...configures, ...events];
    const unique = all.filter((entry) => false /* filled after the global scan */);
    definitions.push({
      file,
      model: raw.model || (raw.zigbeeModel || [])[0] || '',
      vendor: raw.vendor || '',
      extend_only:
        list(raw.extend).length > 0 &&
        list(raw.fromZigbee).length === 0 &&
        list(raw.toZigbee).length === 0 &&
        raw.configure == null &&
        raw.onEvent == null,
      manual: list(raw.fromZigbee).length + list(raw.toZigbee).length > 0,
      converterHashes: converters.map((entry) => entry.hash),
      configureHashes: configures.map((entry) => entry.hash),
      eventHashes: events.map((entry) => entry.hash),
    });
  }
}

// Shared = hash appears in more than one place; unique = exactly one use.
for (const entry of hashes.values()) {
  entry.shared = entry.count > 1;
}

for (const definition of definitions) {
  const all = [
    ...new Set([
      ...definition.converterHashes,
      ...definition.configureHashes,
      ...definition.eventHashes,
    ]),
  ];
  definition.functions = all.length;
  definition.unique_functions = all.filter((hash) => !hashes.get(hash).shared).length;
  definition.shared_functions = definition.functions - definition.unique_functions;
  definition.only_shared = definition.functions > 0 && definition.unique_functions === 0;
  definition.no_functions = definition.functions === 0;
}

const converterEntries = [...hashes.values()].filter((entry) => entry.converters > 0);
const configureEntries = [...hashes.values()].filter((entry) => entry.configures > 0);
const eventEntries = [...hashes.values()].filter((entry) => entry.events > 0);
const topConverters = converterEntries.sort((a, b) => b.count - a.count).slice(0, 30);

const aggregate = {
  files: fileCount,
  definitions: definitions.length,
  extend_only: definitions.filter((d) => d.extend_only).length,
  manual_converters: definitions.filter((d) => d.manual).length,
  no_functions: definitions.filter((d) => d.no_functions).length,
  only_shared: definitions.filter((d) => d.only_shared).length,
  with_unique: definitions.filter((d) => d.unique_functions > 0).length,
  converter_uses: converterEntries.reduce((sum, entry) => sum + entry.count, 0),
  converter_unique_sources: converterEntries.length,
  converter_shared_sources: converterEntries.filter((entry) => entry.shared).length,
  converter_unique_uses: converterEntries.filter((entry) => !entry.shared).reduce((sum, entry) => sum + entry.count, 0),
  configure_uses: configureEntries.reduce((sum, entry) => sum + entry.count, 0),
  configure_unique_sources: configureEntries.length,
  configure_shared_sources: configureEntries.filter((entry) => entry.shared).length,
  event_uses: eventEntries.reduce((sum, entry) => sum + entry.count, 0),
  event_unique_sources: eventEntries.length,
  event_shared_sources: eventEntries.filter((entry) => entry.shared).length,
};

const top = topConverters.map((entry) => ({
  name: entry.name,
  count: entry.count,
  shared: entry.shared,
  clusters: [...entry.clusters.entries()].sort((a, b) => b[1] - a[1]).slice(0, 3),
}));

const report = {
  aggregate,
  top_converters: top,
  top_configure_sources: configureEntries
    .sort((a, b) => b.count - a.count)
    .slice(0, 15)
    .map((entry) => ({ name: entry.name, count: entry.count, shared: entry.shared })),
  definitions,
};
fs.writeFileSync(path.join(__dirname, 'behavior_report.json'), JSON.stringify(report, null, 1));

console.log(JSON.stringify(aggregate, null, 2));
console.log('top converters:');
for (const entry of top) {
  console.log(`  ${entry.count}\t${entry.name}\t${entry.clusters.map(([c, n]) => `${c}:${n}`).join(',')}`);
}
