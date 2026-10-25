'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { packageRoot, devicesDir, list } = require('./zhc.cjs');
const { pkg } = require('./cli.cjs');

const { findDefinition, setLogger } = require(path.join(pkg, 'dist/index.js'));
setLogger({ debug() {}, info() {}, warn() {}, error() {} });
const modelsIndex = JSON.parse(fs.readFileSync(path.join(pkg, 'dist/models-index.json'), 'utf8'));
const manifest = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../out/catalog_manifest.json'), 'utf8'));

function normalize(model) {
  return model.replace(/\0(.|\n)*$/g, '').trim();
}

function definitionAt(file, index) {
  return require(path.join(devicesDir, file)).definitions[index];
}

function candidates(modelID) {
  const key = modelID.toLowerCase();
  return (modelsIndex[key] ?? modelsIndex[normalize(key)] ?? []).map(([file, index]) => ({
    file,
    index,
    definition: definitionAt(file, index),
  }));
}

function sourceOf(selected, modelID) {
  if (!selected) return null;
  for (const candidate of candidates(modelID)) {
    if (candidate.definition === selected) return candidate;
  }
  return null;
}

function regexValue(regexp) {
  if (!regexp || typeof regexp !== 'object' || !regexp.regex) return null;
  const source = regexp.regex;
  if (!source.startsWith('^') || !source.endsWith('$')) return null;
  const body = source.slice(1, -1);
  if (!/^0x[0-9a-fA-F.]+$/.test(body)) return null;
  return body.replace(/\./g, '0');
}

function device(fingerprint) {
  const endpoints = list(fingerprint.endpoints).map((endpoint) => ({
    ID: endpoint.ID,
    profileID: endpoint.profileID,
    deviceID: endpoint.deviceID,
    inputClusters: endpoint.inputClusters ?? [],
    outputClusters: endpoint.outputClusters ?? [],
  }));
  const identity = {
    type: fingerprint.type ?? 'Router',
    modelID: fingerprint.modelID,
    manufacturerName: fingerprint.manufacturerName,
    manufacturerID: fingerprint.manufacturerID,
    powerSource: fingerprint.powerSource,
    softwareBuildID: fingerprint.softwareBuildID,
    dateCode: fingerprint.dateCode,
    applicationVersion: fingerprint.applicationVersion,
    hardwareVersion: fingerprint.hardwareVersion,
    zclVersion: fingerprint.zclVersion,
    endpoints: endpoints.length ? endpoints : [{ ID: 1, inputClusters: [], outputClusters: [] }],
  };
  const ieee = regexValue(fingerprint.ieeeAddr);
  if (ieee) identity.ieeeAddr = ieee;
  identity.getEndpoint = (id) => identity.endpoints.find((endpoint) => endpoint.ID === id);
  return identity;
}

const categories = {};
const examples = [];
function note(category, item) {
  const bucket = (categories[category] ??= { cases: 0, matched: 0, different: 0, missing: 0 });
  bucket.cases += 1;
  if (item.result === 'matched') bucket.matched += 1;
  else if (item.result === 'different') bucket.different += 1;
  else bucket.missing += 1;
  if (item.result !== 'matched' && examples.length < 20) examples.push({ category, ...item });
}

async function check(category, fingerprint, expected, model) {
  if (!model) return;
  const selected = await findDefinition(device(fingerprint), false);
  const source = sourceOf(selected, model);
  if (!source) {
    note(category, { result: 'missing', model, expected });
    return;
  }
  const sourceId = `${source.file}#${source.index}`;
  note(category, {
    result: sourceId === expected ? 'matched' : 'different',
    model,
    expected,
    selected: sourceId,
  });
}

(async () => {
  for (const definition of manifest.definitions) {
    const expected = `${definition.file}#${definition.index}`;
    for (const fingerprint of list(definition.fingerprint)) {
      if (!fingerprint || !fingerprint.modelID) continue;
      const flags = [];
      if (fingerprint.endpoints) flags.push('endpoints');
      if (fingerprint.ieeeAddr) flags.push('regex');
      if (fingerprint.priority !== undefined) flags.push('priority');
      for (const flag of flags) await check(flag, fingerprint, expected, fingerprint.modelID);
    }
    for (const whiteLabel of list(definition.whiteLabel)) {
      for (const fingerprint of list(whiteLabel && whiteLabel.fingerprint)) {
        if (!fingerprint || !fingerprint.modelID) continue;
        await check('whiteLabel', fingerprint, expected, fingerprint.modelID);
      }
    }
  }

  const report = { package: manifest.package?.version ?? '26.123.0', categories, examples };
  fs.writeFileSync(path.resolve(__dirname, '../out/identify_complex_report.json'), JSON.stringify(report, null, 1));
  for (const [category, bucket] of Object.entries(categories)) {
    console.log(`${category}: ${bucket.cases} cases, ${bucket.matched} matched, ${bucket.different} different, ${bucket.missing} missing`);
  }
  if (examples.length) {
    console.log('examples:');
    for (const item of examples.slice(0, 10)) {
      console.log(`  ${item.category} ${item.model}: ${item.expected} -> ${item.selected ?? 'missing'}`);
    }
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});