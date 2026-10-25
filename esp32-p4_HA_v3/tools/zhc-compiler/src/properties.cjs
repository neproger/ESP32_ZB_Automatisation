'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { packageRoot, devicesDir, listFiles, list } = require('./zhc.cjs');
const { pkg } = require('./cli.cjs');

const { prepareDefinition } = require(path.join(pkg, 'dist/index.js'));

// V2 canonical property IDs from ha_model/include/ha_model/ha_properties.h.
const V2 = {
  POWER: 1,
  BRIGHTNESS: 2,
  COLOR_HUE: 3,
  COLOR_SATURATION: 4,
  COLOR_X: 5,
  COLOR_Y: 6,
  COLOR_TEMPERATURE: 7,
  TEMPERATURE: 8,
  HUMIDITY: 9,
  ILLUMINANCE: 10,
  OCCUPANCY: 11,
  BATTERY_VOLTAGE: 12,
  BATTERY_PERCENT: 13,
  COLOR: 22,
};
const V3 = new Set([V2.POWER, V2.BRIGHTNESS, V2.COLOR_HUE, V2.COLOR_SATURATION, V2.COLOR_X, V2.COLOR_Y, V2.COLOR_TEMPERATURE, V2.COLOR]);

const MAP = {
  state: { id: V2.POWER, kind: 'bool' },
  brightness: { id: V2.BRIGHTNESS, kind: 'percent' },
  color_hue: { id: V2.COLOR_HUE, kind: 'float' },
  color_saturation: { id: V2.COLOR_SATURATION, kind: 'percent' },
  color_x: { id: V2.COLOR_X, kind: 'float' },
  color_y: { id: V2.COLOR_Y, kind: 'float' },
  color_temp: { id: V2.COLOR_TEMPERATURE, kind: 'kelvin' },
  color: { id: V2.COLOR, kind: 'xy' },
  temperature: { id: V2.TEMPERATURE, kind: 'celsius' },
  humidity: { id: V2.HUMIDITY, kind: 'percent' },
  illuminance: { id: V2.ILLUMINANCE, kind: 'lux' },
  occupancy: { id: V2.OCCUPANCY, kind: 'bool' },
  battery: { id: V2.BATTERY_PERCENT, kind: 'percent' },
};

const counts = new Map();
const definitions = new Map();
let skipped = 0;
let exposeCount = 0;

function walkExposes(value, propertyCounts, seen) {
  for (const expose of list(value)) {
    if (!expose || typeof expose !== 'object') continue;
    if (expose.property) {
      exposeCount += 1;
      const name = String(expose.property);
      counts.set(name, (counts.get(name) || 0) + 1);
      propertyCounts.set(name, true);
    }
    for (const feature of list(expose.features)) {
      if (feature) {
        if (feature.property) {
          exposeCount += 1;
          const name = String(feature.property);
          counts.set(name, (counts.get(name) || 0) + 1);
          propertyCounts.set(name, true);
        }
      }
    }
  }
}

for (const file of listFiles()) {
  const module = require(path.join(devicesDir, file));
  for (const raw of list(module.definitions)) {
    let prepared;
    try {
      prepared = prepareDefinition(raw);
    } catch {
      skipped += 1;
      continue;
    }
    const propertyCounts = new Map();
    walkExposes(prepared.exposes, propertyCounts, definitions);
    for (const name of propertyCounts.keys()) definitions.set(name, (definitions.get(name) || 0) + 1);
  }
}

const rows = [...counts.entries()]
  .sort((a, b) => b[1] - a[1])
  .map(([property, occurrences]) => {
    const mapped = MAP[property];
    return {
      property,
      occurrences,
      definitions: definitions.get(property) || 0,
      v2_id: mapped ? mapped.id : null,
      v2_kind: mapped ? mapped.kind : null,
      v3_present: mapped ? V3.has(mapped.id) : false,
      status: mapped ? (V3.has(mapped.id) ? 'v3' : 'v2_only') : 'missing',
    };
  });

const report = {
  package: '26.123.0',
  source: 'prepared ZHC exposes',
  totals: { exposes: exposeCount, skipped_definitions: skipped, distinct_properties: rows.length },
  v2_ids: V2,
  v3_ids: [...V3],
  rows,
};
fs.writeFileSync(path.resolve(__dirname, '../out/property_audit.json'), JSON.stringify(report, null, 1));

for (const row of rows.slice(0, 30)) {
  console.log(
    `${row.status}\tV2=${row.v2_id ?? '-'}\tdefs=${row.definitions}\texposes=${row.occurrences}\t${row.property}`,
  );
}
console.log(`distinct properties: ${rows.length}, exposes: ${exposeCount}, skipped defs: ${skipped}`);