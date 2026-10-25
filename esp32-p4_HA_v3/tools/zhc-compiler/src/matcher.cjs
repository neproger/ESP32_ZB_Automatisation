'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { packageRoot, list } = require('./zhc.cjs');
const { pkg } = require('./cli.cjs');

const manifest = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../out/catalog_manifest.json'), 'utf8'));
const modelsIndex = JSON.parse(fs.readFileSync(path.join(pkg, 'dist/models-index.json'), 'utf8'));
const lighting = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../../z2m_spike/lighting_manifest.json'), 'utf8'));

const profileBySource = new Map();
for (const [index, profile] of lighting.profiles.entries()) {
  if (profile.source) {
    profileBySource.set(`${profile.source.file}#${profile.source.definition_index}`, `ZHCL_${index}`);
  }
}
profileBySource.set('tuya.js#150', 'TUYA_TS0041');
profileBySource.set('sonoff.js#25', 'SNZB_01P');
profileBySource.set('tuya.js#229', 'TS011F');
const sensorsFile = path.resolve(__dirname, '../out/sensors_manifest.json');
if (fs.existsSync(sensorsFile)) {
  const sensors = JSON.parse(fs.readFileSync(sensorsFile, 'utf8'));
  for (const profile of sensors.profiles) {
    profileBySource.set(`${profile.source.file}#${profile.source.definition_index}`, profile.symbol);
  }
}

const bySource = new Map();
manifest.definitions.forEach((definition, index) => {
  bySource.set(`${definition.file}#${definition.index}`, index);
});

function quote(value) {
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
const opt = (value, render) => (value === null || value === undefined ? 'None' : `Some(${render(value)})`);

function printf(value) {
  return value === null || value === undefined ? 'None' : `Some(${Number(value)})`;
}

function regexRule(rule) {
  if (!rule || typeof rule !== 'object' || !rule.regex) return 'None';
  const source = rule.regex;
  if (!source.startsWith('^') || !source.endsWith('$')) throw new Error(`unsupported ieeeAddr regex ${source}`);
  const body = source.slice(1, -1);
  if (!/^0x[0-9a-fA-F.]+$/.test(body)) throw new Error(`unsupported ieeeAddr regex ${source}`);
  const wildcards = (body.match(/\./g) || []).length;
  const prefix = body.replace(/\./g, '');
  return `Some(IeeeRule { prefix: ${quote(prefix)}, wildcards: ${wildcards} })`;
}

const endpointStatics = [];
function endpointStatic(definitionIndex, fingerprintIndex, endpoints) {
  const name = `EP_${definitionIndex}_${fingerprintIndex}`;
  const lines = [`static ${name}: &[EndpointRule] = &[`];
  for (const endpoint of endpoints) {
    const input = list(endpoint.inputClusters).map((value) => `${value}`).join(', ');
    const output = list(endpoint.outputClusters).map((value) => `${value}`).join(', ');
    lines.push(
      `    EndpointRule { id: ${printf(endpoint.ID)}, device_id: ${printf(endpoint.deviceID)}, ` +
        `profile_id: ${printf(endpoint.profileID)}, input: &[${input}], output: &[${output}] },`,
    );
  }
  lines.push('];');
  endpointStatics.push(...lines);
  return name;
}

const fingerprintStatics = [];
manifest.definitions.forEach((definition, definitionIndex) => {
  const lines = [`static FP_${definitionIndex}: &[FingerprintRule] = &[`];
  const fingerprints = list(definition.fingerprint);
  fingerprints.forEach((fingerprint, fingerprintIndex) => {
    const endpoints = fingerprint.endpoints
      ? `Some(${endpointStatic(definitionIndex, fingerprintIndex, fingerprint.endpoints)})`
      : 'None';
    lines.push(
      `    FingerprintRule { manufacturer_name: ${opt(fingerprint.manufacturerName, quote)}, ` +
        `model_id: ${opt(fingerprint.modelID, quote)}, ` +
        `manufacturer_id: ${printf(fingerprint.manufacturerID)}, ` +
        `type_: ${opt(fingerprint.type, quote)}, power_source: ${opt(fingerprint.powerSource, quote)}, ` +
        `software_build_id: ${opt(fingerprint.softwareBuildID, quote)}, ` +
        `date_code: ${opt(fingerprint.dateCode, quote)}, ` +
        `application_version: ${printf(fingerprint.applicationVersion)}, ` +
        `hardware_version: ${printf(fingerprint.hardwareVersion)}, zcl_version: ${printf(fingerprint.zclVersion)}, ` +
        `priority: ${Number(fingerprint.priority ?? 0)}, ieee: ${regexRule(fingerprint.ieeeAddr)}, endpoints: ${endpoints} },`,
    );
  });
  lines.push('];');
  fingerprintStatics.push(...lines);
});

const definitionLines = ['pub static DEFINITIONS: &[Definition] = &['];
manifest.definitions.forEach((definition, definitionIndex) => {
  const source = `${definition.file}#${definition.index}`;
  const profile = profileBySource.get(source);
  const models = list(definition.zigbeeModel).map(quote).join(', ');
  const whiteLabels = list(definition.whiteLabel)
    .map(
      (whiteLabel) =>
        `WhiteLabel { model: ${quote(whiteLabel.model)}, vendor: ${quote(whiteLabel.vendor)}, ` +
        `description: ${opt(whiteLabel.description, quote)} }`,
    )
    .join(', ');
  definitionLines.push(
    `    Definition { id: ${quote(definition.id)}, source: ${quote(source)}, model: ${quote(definition.model)}, ` +
      `vendor: ${quote(definition.vendor)}, profile: ${profile ? `Some(&${profile})` : 'None'}, ` +
      `zigbee_models: &[${models}], fingerprints: FP_${definitionIndex}, white_labels: &[${whiteLabels}] },`,
  );
});
definitionLines.push('];');

const keys = Object.keys(modelsIndex).sort();
const candidateStatics = [];
const candidateRows = [];
keys.forEach((key, index) => {
  const candidates = modelsIndex[key].map(([file, definitionIndex]) => {
    const mapped = bySource.get(`${file}#${definitionIndex}`);
    if (mapped === undefined) throw new Error(`missing definition ${file}#${definitionIndex}`);
    return mapped;
  });
  candidateStatics.push(`static CAND_${index}: &[u16] = &[${candidates.join(', ')}];`);
  candidateRows.push(`CAND_${index}`);
});

const output = [
  '// @generated by tools/zhc-compiler/src/matcher.cjs from ZHC 26.123.0.',
  'use crate::matcher::{Definition, EndpointRule, FingerprintRule, IeeeRule, WhiteLabel};',
  'use crate::{SNZB_01P, TS011F, TUYA_TS0041};',
  `use super::{${[...new Set(profileBySource.values())]
    .filter((name) => name.startsWith('ZHCL_') || name.startsWith('SENSOR_'))
    .sort()
    .join(', ')}};`,
  '',
  ...endpointStatics,
  '',
  ...fingerprintStatics,
  '',
  ...definitionLines,
  '',
  ...candidateStatics,
  '',
  `pub static MODEL_KEYS: &[&str] = &[${keys.map(quote).join(', ')}];`,
  'pub static MODEL_CANDIDATES: &[&[u16]] = &[',
  ...candidateRows.map((row) => `    ${row},`),
  '];',
  '',
];

fs.writeFileSync(path.resolve(__dirname, '../../../crates/z2m-rs/src/generated/matcher_data.rs'), output.join('\n'));
console.log(`matcher definitions: ${manifest.definitions.length}, model keys: ${keys.length}, profiles: ${profileBySource.size}`);