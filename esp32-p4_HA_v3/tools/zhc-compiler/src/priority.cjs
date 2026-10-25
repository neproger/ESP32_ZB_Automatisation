'use strict';

const fs = require('node:fs');
const path = require('node:path');

const coverage = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../out/coverage_manifest.json'), 'utf8'));
const rows = new Map();

function row(key) {
  const entry = rows.get(key) ?? { key, uses: 0, unblocks_partial: 0, unblocks_supported: 0 };
  rows.set(key, entry);
  return entry;
}

for (const definition of coverage.definitions) {
  const unsupported = [...new Set(definition.items.filter((item) => item.status === 'unsupported').map((item) => `${item.kind}:${item.helper}`))];
  const partial = [...new Set(definition.items.filter((item) => item.status === 'partial').map((item) => `${item.kind}:${item.helper}`))];
  for (const key of unsupported) row(key).uses += 1;
  if (unsupported.length === 1) {
    const entry = row(unsupported[0]);
    if (definition.status === 'UNSUPPORTED') entry.unblocks_partial += 1;
    else if (definition.status === 'PARTIAL' && partial.length === 0) entry.unblocks_supported += 1;
  }
}

const ranked = [...rows.values()]
  .filter((entry) => entry.uses > 0)
  .sort((a, b) => {
    const aGain = a.unblocks_partial + a.unblocks_supported;
    const bGain = b.unblocks_partial + b.unblocks_supported;
    return bGain - aGain || b.uses - a.uses;
  });

const output = {
  package: coverage.package,
  generated_from: 'out/coverage_manifest.json',
  note: 'Unblocks counts definitions whose only remaining unsupported mechanism is this key.',
  ranked,
};
fs.writeFileSync(path.resolve(__dirname, '../out/mechanism_priority.json'), JSON.stringify(output, null, 1));

for (const entry of ranked.slice(0, 20)) {
  console.log(
    `${entry.unblocks_partial}+${entry.unblocks_supported}\tuses=${entry.uses}\t${entry.key}`,
  );
}