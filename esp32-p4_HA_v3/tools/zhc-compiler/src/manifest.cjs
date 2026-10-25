'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { packageJson, packageRoot, devicesDir, listFiles, list, plain } = require('./zhc.cjs');

const EXPECTED_VERSION = '26.123.0';
const EXPECTED_FILES = 401;
const EXPECTED_DEFINITIONS = 4595;
const outDir = path.join(__dirname, '..', 'out');

function hash(value) {
  return crypto.createHash('sha256').update(value).digest('hex').slice(0, 32);
}

function identity(file, index, matching) {
  return hash(`${file}#${index}\n${JSON.stringify(matching)}`);
}

const definitions = [];
const bySource = new Map();

for (const file of listFiles()) {
  const module = require(path.join(devicesDir, file));
  list(module.definitions).forEach((raw, index) => {
    const matching = {
      model: raw.model ?? null,
      vendor: raw.vendor ?? null,
      manufacturerName: plain(raw.manufacturerName ?? null),
      zigbeeModel: plain(raw.zigbeeModel ?? null),
      fingerprint: plain(raw.fingerprint ?? null),
      whiteLabel: plain(raw.whiteLabel ?? null),
    };
    const entry = {
      id: identity(file, index, matching),
      file,
      index,
      model: raw.model ?? null,
      vendor: raw.vendor ?? null,
      ...matching,
      lookup_keys: [],
    };
    definitions.push(entry);
    bySource.set(`${file}#${index}`, entry);
  });
}

const modelsIndexPath = path.join(packageRoot, 'dist', 'models-index.json');
const modelsIndex = JSON.parse(fs.readFileSync(modelsIndexPath, 'utf8'));
const lookup = {};
const missing = [];
const keysById = new Map();

for (const key of Object.keys(modelsIndex).sort()) {
  lookup[key] = modelsIndex[key].map(([file, index]) => {
    const entry = bySource.get(`${file}#${index}`);
    if (!entry) {
      missing.push({ key, file, index });
      return { file, index, missing: true };
    }
    if (!keysById.has(entry.id)) keysById.set(entry.id, []);
    keysById.get(entry.id).push(key);
    return { id: entry.id, file, index, model: entry.model, vendor: entry.vendor };
  });
}
for (const entry of definitions) entry.lookup_keys = keysById.get(entry.id) ?? [];

const uniqueIds = new Set(definitions.map((entry) => entry.id));
const files = listFiles();
const manifest = {
  schema: 1,
  package: {
    name: packageJson.name,
    version: packageJson.version,
    engines: packageJson.engines,
    lockfile: 'package-lock.json',
  },
  counts: {
    files: files.length,
    definitions: definitions.length,
    unique_identities: uniqueIds.size,
    lookup_models: Object.keys(lookup).length,
    index_candidates: Object.values(lookup).reduce((sum, list) => sum + list.length, 0),
    missing_candidates: missing.length,
  },
  selection: {
    index_key: 'modelID.toLowerCase(); fallback normalizeModelID (strip NUL tail, trim)',
    candidate_order: 'models-index.json array order',
    fingerprint_fields: [
      'applicationVersion',
      'manufacturerID',
      'type',
      'dateCode',
      'hardwareVersion',
      'manufacturerName',
      'modelID',
      'powerSource',
      'softwareBuildID',
      'stackVersion',
      'zclVersion',
      'ieeeAddr (regex)',
      'endpoints',
      'priority',
    ],
    fingerprint_choice: 'highest numeric priority wins; first candidate on ties',
    fallback: 'first candidate whose zigbeeModel equals modelID or normalized modelID',
    white_label: 'fingerprint match in whiteLabel replaces model/vendor/description',
  },
  models_index: lookup,
  definitions,
};

if (manifest.package.version !== EXPECTED_VERSION) {
  console.error(`expected ZHC ${EXPECTED_VERSION}, installed ${manifest.package.version}`);
  process.exit(1);
}
if (manifest.counts.files !== EXPECTED_FILES || manifest.counts.definitions !== EXPECTED_DEFINITIONS) {
  console.error(
    `expected ${EXPECTED_FILES} files / ${EXPECTED_DEFINITIONS} definitions, ` +
      `got ${manifest.counts.files} / ${manifest.counts.definitions}`,
  );
  process.exit(1);
}
if (missing.length > 0) {
  console.error(`models-index references ${missing.length} definitions not present in dist/devices`);
  process.exit(1);
}
if (uniqueIds.size !== definitions.length) {
  console.error(`identity collision: ${definitions.length} definitions, ${uniqueIds.size} ids`);
  process.exit(1);
}

fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, 'catalog_manifest.json'), JSON.stringify(manifest));

console.log(
  `ZHC ${manifest.package.version}: ${manifest.counts.files} files, ` +
    `${manifest.counts.definitions} definitions, ${manifest.counts.lookup_models} model keys`,
);
console.log(`manifest: ${path.relative(process.cwd(), path.join(outDir, 'catalog_manifest.json'))}`);