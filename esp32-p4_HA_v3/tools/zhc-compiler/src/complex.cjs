'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { packageRoot, devicesDir, list } = require('./zhc.cjs');
const { pkg } = require('./cli.cjs');

const { findDefinition, setLogger } = require(path.join(pkg, 'dist/index.js'));
setLogger({ debug() {}, info() {}, warn() {}, error() {} });
const modelsIndex = JSON.parse(fs.readFileSync(path.join(pkg, 'dist/models-index.json'), 'utf8'));
const manifest = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../out/catalog_manifest.json'), 'utf8'));
const idBySource = new Map(
  manifest.definitions.map((definition) => [`${definition.file}#${definition.index}`, definition.id]),
);

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
const cases = [];
const seenCases = new Set();
function note(category, item) {
  const bucket = (categories[category] ??= { cases: 0, matched: 0, different: 0, missing: 0 });
  bucket.cases += 1;
  if (item.result === 'matched') bucket.matched += 1;
  else if (item.result === 'different') bucket.different += 1;
  else bucket.missing += 1;
  if (item.result !== 'matched' && examples.length < 20) examples.push({ category, ...item });
}

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

function option(value, render) {
  return value === null || value === undefined || value === '' ? 'None' : `Some(${render(value)})`;
}

function caseLiteral(category, fingerprint, expectedId) {
  const endpoints = list(fingerprint.endpoints)
    .map(
      (endpoint) =>
        `EndpointIdentity { id: ${endpoint.ID ?? 0}, device_id: ${
          endpoint.deviceID === undefined ? 'None' : `Some(${endpoint.deviceID})`
        }, ` +
        `profile_id: ${
          endpoint.profileID === undefined ? 'None' : `Some(${endpoint.profileID})`
        }, input: &[${list(endpoint.inputClusters).join(', ')}], ` +
        `output: &[${list(endpoint.outputClusters).join(', ')}] }`,
    )
    .join(', ');
  const ieee = regexValue(fingerprint.ieeeAddr);
  return (
    `    Case { category: ${rustString(category)}, model: ${rustString(fingerprint.modelID)}, ` +
    `manufacturer_name: ${option(fingerprint.manufacturerName, rustString)}, ` +
    `manufacturer_id: ${option(fingerprint.manufacturerID, (value) => value)}, ` +
    `type_: ${option(fingerprint.type, rustString)}, power_source: ${option(fingerprint.powerSource, rustString)}, ` +
    `software_build_id: ${option(fingerprint.softwareBuildID, rustString)}, ` +
    `date_code: ${option(fingerprint.dateCode, rustString)}, ` +
    `application_version: ${option(fingerprint.applicationVersion, (value) => value)}, ` +
    `hardware_version: ${option(fingerprint.hardwareVersion, (value) => value)}, ` +
    `zcl_version: ${option(fingerprint.zclVersion, (value) => value)}, ` +
    `ieee: ${option(ieee, rustString)}, endpoints: &[${endpoints}], expected: ${rustString(expectedId)} },`
  );
}

async function check(category, fingerprint, expected, model, expectedId) {
  if (!model) return;
  if (!model) return;
  const selected = await findDefinition(device(fingerprint), false);
  const source = sourceOf(selected, model);
  if (!source) {
    note(category, { result: 'missing', model, expected });
    return;
  }
  const sourceId = `${source.file}#${source.index}`;
  const matched = sourceId === expected;
  note(category, {
    result: matched ? 'matched' : 'different',
    model,
    expected,
    selected: sourceId,
  });
}

async function emitSelected(category, fingerprint) {
  if (!fingerprint || !fingerprint.modelID) return;
  const selected = await findDefinition(device(fingerprint), false);
  const source = sourceOf(selected, fingerprint.modelID);
  if (!source) {
    note(category, { result: 'missing', model: fingerprint.modelID, expected: 'unknown' });
    return;
  }
  const sourceKey = `${source.file}#${source.index}`;
  const expectedId = idBySource.get(sourceKey);
  if (!expectedId) throw new Error(`selected definition missing from manifest: ${sourceKey}`);
  note(category, { result: 'matched', model: fingerprint.modelID, expected: sourceKey, selected: sourceKey });
  const key = JSON.stringify(fingerprint);
  if (!seenCases.has(key)) {
    seenCases.add(key);
    cases.push(caseLiteral(category, fingerprint, expectedId));
  }
}

(async () => {
  for (const definition of manifest.definitions) {
    const expected = `${definition.file}#${definition.index}`;
    for (const fingerprint of list(definition.fingerprint)) {
      if (!fingerprint || !fingerprint.modelID) continue;
      await emitSelected('all', fingerprint);
      const flags = [];
      if (fingerprint.endpoints) flags.push('endpoints');
      if (fingerprint.ieeeAddr) flags.push('regex');
      if (fingerprint.priority !== undefined) flags.push('priority');
      for (const flag of flags) await check(flag, fingerprint, expected, fingerprint.modelID, definition.id);
    }
    for (const whiteLabel of list(definition.whiteLabel)) {
      for (const fingerprint of list(whiteLabel && whiteLabel.fingerprint)) {
        if (!fingerprint || !fingerprint.modelID) continue;
        await emitSelected('whiteLabel', fingerprint);
        await check('whiteLabel', fingerprint, expected, fingerprint.modelID, definition.id);
      }
    }
  }

  const report = { package: manifest.package?.version ?? '26.123.0', categories, examples };
  fs.writeFileSync(path.resolve(__dirname, '../out/identify_complex_report.json'), JSON.stringify(report, null, 1));
  const caseFile = path.resolve(__dirname, '../../../crates/z2m-rs/tests/golden/matcher_complex_cases.rs');
  fs.mkdirSync(path.dirname(caseFile), { recursive: true });
  fs.writeFileSync(
    caseFile,
    `use z2m_rs::EndpointIdentity;

struct Case {
    category: &'static str,
    model: &'static str,
    manufacturer_name: Option<&'static str>,
    manufacturer_id: Option<u16>,
    type_: Option<&'static str>,
    power_source: Option<&'static str>,
    software_build_id: Option<&'static str>,
    date_code: Option<&'static str>,
    application_version: Option<u16>,
    hardware_version: Option<u16>,
    zcl_version: Option<u16>,
    ieee: Option<&'static str>,
    endpoints: &'static [EndpointIdentity<'static>],
    expected: &'static str,
}

static CASES: &[Case] = &[
${cases.join('\n')}
];
`
  );
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