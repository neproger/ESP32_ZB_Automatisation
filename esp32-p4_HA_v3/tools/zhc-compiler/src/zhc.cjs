'use strict';

const fs = require('node:fs');
const path = require('node:path');

const [nodeMajor, nodeMinor] = process.versions.node.split('.').map(Number);
if (nodeMajor < 22 || (nodeMajor === 22 && nodeMinor < 12)) {
  console.warn(
    `Node ${process.versions.node} is unsupported for reproducible runs; use >=22.12 (see package.json)`,
  );
}

const packageFile = require.resolve('zigbee-herdsman-converters/package.json');
const packageRoot = path.dirname(packageFile);
const packageJson = JSON.parse(fs.readFileSync(packageFile, 'utf8'));

function listFiles() {
  return fs
    .readdirSync(path.join(packageRoot, 'dist', 'devices'))
    .filter((file) => file.endsWith('.js'))
    .sort();
}

function list(value) {
  return Array.isArray(value) ? value : value == null ? [] : [value];
}

// JSON-safe form of ZHC data: regexes keep their source, functions are named.
function plain(value) {
  if (value instanceof RegExp) return { regex: value.source, flags: value.flags };
  if (typeof value === 'function') return '[fn]';
  if (Array.isArray(value)) return value.map(plain);
  if (value && typeof value === 'object') {
    const out = {};
    for (const key of Object.keys(value).sort()) out[key] = plain(value[key]);
    return out;
  }
  return value;
}

module.exports = {
  packageJson,
  packageRoot,
  devicesDir: path.join(packageRoot, 'dist', 'devices'),
  listFiles,
  list,
  plain,
};