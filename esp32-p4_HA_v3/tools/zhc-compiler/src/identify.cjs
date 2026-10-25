'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { packageRoot, devicesDir, list } = require('./zhc.cjs');
const { pkg } = require('./cli.cjs');

const { findDefinition, setLogger } = require(path.join(pkg, 'dist/index.js'));
setLogger({ debug() {}, info() {}, warn() {}, error() {} });
const modelsIndex = JSON.parse(fs.readFileSync(path.join(pkg, 'dist/models-index.json'), 'utf8'));
const lighting = JSON.parse(fs.readFileSync(path.join(__dirname, '..', '..', 'z2m_spike', 'lighting_manifest.json'), 'utf8'));

function normalizeModelID(modelID) {
  return modelID.replace(/\0(.|\n)*$/g, '').trim();
}

function definitionAt(file, index) {
  return require(path.join(devicesDir, file)).definitions[index];
}

function candidates(modelID) {
  const key = modelID.toLowerCase();
  const list = modelsIndex[key] ?? modelsIndex[normalizeModelID(key)] ?? [];
  return list.map(([file, index]) => ({ file, index, definition: definitionAt(file, index) }));
}

async function sourceOf(selected, modelID) {
  if (!selected) return null;
  for (const candidate of candidates(modelID)) {
    if (candidate.definition === selected) return candidate;
  }
  return null;
}

function device(modelID, manufacturerName) {
  return {
    type: 'Router',
    modelID,
    manufacturerName: manufacturerName || undefined,
    powerSource: 'Mains (single phase)',
    ieeeAddr: '0x00124b0000000000',
    endpoints: [{ ID: 1, inputClusters: [], outputClusters: [] }],
  };
}

const fingerprintCounts = new Map();
for (const profile of lighting.profiles) {
  for (const fingerprint of profile.fingerprints) {
    const key = `${normalizeModelID(fingerprint.model)}\0${fingerprint.manufacturer || ''}`;
    fingerprintCounts.set(key, (fingerprintCounts.get(key) || 0) + 1);
  }
}
const collisions = [];
const rows = [];
const shadowed = [];
const missingSource = [];
const seen = new Set();

async function addCase(profile, model, manufacturer, expected, label) {
  if (!model) return;
  if (/[\x01-\x08\x0b\x0c\x0e-\x1f]/.test(model)) return;
  const fingerprintKey = `${normalizeModelID(model)}\0${manufacturer || ''}`;
  if ((fingerprintCounts.get(fingerprintKey) || 0) > 1) {
    if (!collisions.some((item) => item.key === fingerprintKey)) {
      collisions.push({ key: fingerprintKey, model, manufacturer: manufacturer || '' });
    }
    return;
  }
  const key = `${model}\t${manufacturer || ''}\t${expected}`;
  if (seen.has(key)) return;
  seen.add(key);
  const selected = await findDefinition(device(model, manufacturer), false);
  const source = await sourceOf(selected, model);
  if (!source) {
    missingSource.push({ profile: profile.id, model, manufacturer, label });
    return;
  }
  if (source.file !== profile.source.file || source.index !== profile.source.definition_index) {
    shadowed.push({
      profile: profile.id,
      model,
      manufacturer,
      selected: `${source.file}#${source.index}`,
      label,
    });
    return;
  }
  rows.push({ model, manufacturer: manufacturer || '', profile: expected });
}

(async () => {
  for (const profile of lighting.profiles) {
    for (const fingerprint of profile.fingerprints) {
      await addCase(profile, fingerprint.model, fingerprint.manufacturer, profile.id, 'fingerprint');
    }
    const first = profile.fingerprints[0];
    if (first) {
      await addCase(profile, `${first.model}\0suffix`, first.manufacturer, profile.id, 'normalized');
    }
  }

  const out = path.resolve(__dirname, '../../../crates/z2m-rs/tests/golden/identify.tsv');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(
    out,
    [
      'model\tmanufacturer\tprofile',
      ...rows.map(
        (row) =>
          `${row.model.replace(/\0/g, '\\0')}\t${row.manufacturer.replace(/\0/g, '\\0')}\t${row.profile}`,
      ),
    ].join('\n') + '\n',
  );
  const report = { profiles: lighting.profiles.length, cases: rows.length, shadowed, missingSource, collisions };
  fs.writeFileSync(path.resolve(__dirname, '../out/identify_report.json'), JSON.stringify(report, null, 1));
  console.log(`identify cases: ${rows.length}, shadowed: ${shadowed.length}, missing source: ${missingSource.length}`);
  console.log(`golden: ${path.relative(process.cwd(), out)}`);
  if (shadowed.length) {
    console.log('shadowed examples:');
    for (const item of shadowed.slice(0, 8)) {
      console.log(`  ${item.profile} ${item.model}/${item.manufacturer || '*'} -> ${item.selected} (${item.label})`);
    }
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});