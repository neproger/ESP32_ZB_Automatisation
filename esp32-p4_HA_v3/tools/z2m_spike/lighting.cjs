// Lighting family inventory: what a standard on/off + brightness + color-temp
// template would cover, and what blocks the rest.
//
// usage: node lighting.cjs [zhc-package] [dist/devices-dir]
// Defaults come from tools/zhc-compiler's pinned npm dependency.
const fs = require('node:fs');
const path = require('node:path');
const { pkg, devicesDir } = require('../zhc-compiler/src/cli.cjs');
const fz = require(path.join(pkg, 'dist/converters/fromZigbee'));
const tz = require(path.join(pkg, 'dist/converters/toZigbee'));
const list = (value) => (Array.isArray(value) ? value : value == null ? [] : [value]);

const FZ_KNOWN = new Set([fz.on_off, fz.brightness, fz.color_colortemp]);
const TZ_KNOWN = new Set([tz.on_off, tz.light_onoff_brightness, tz.light_colortemp]);
const CONFIGURE_BAD = ['endpoint.read', 'saveClusterAttributeKeyValue', 'read(', 'command('];
const CONFIGURE_GOOD = /reporting(_\d+)?\.|\.bind\(|\.onOff\(|configureMagicPacket/;

function lightExpose(raw) {
  for (const item of [...list(raw.exposes), ...list(raw.extend).flatMap((e) => list(e.exposes))]) {
    if (item && item.type === 'light') return item;
  }
  return null;
}

function features(light) {
  const names = new Set();
  for (const feature of list(light.features)) if (feature && feature.name) names.add(feature.name);
  // Older ZHC exposes a flat list without features.
  for (const key of ['brightness', 'color_temp', 'color', 'color_hs', 'color_xy']) {
    if (light[key]) names.add(key);
  }
  return names;
}

function converters(raw) {
  const from = [...list(raw.fromZigbee), ...list(raw.extend).flatMap((e) => list(e.fromZigbee))];
  const to = [...list(raw.toZigbee), ...list(raw.extend).flatMap((e) => list(e.toZigbee))];
  return { from, to };
}

function configureOk(raw) {
  const values = [...list(raw.configure), ...list(raw.extend).flatMap((e) => list(e.configure))];
  const functions = values.filter((v) => typeof v === 'function');
  if (functions.length === 0) return true;
  for (const fn of functions) {
    if (fn === require(path.join(pkg, 'dist/lib/tuya')).configureMagicPacket) continue;
    const src = fn.toString();
    if (CONFIGURE_BAD.some((needle) => src.includes(needle))) return false;
    if (!CONFIGURE_GOOD.test(src)) return false;
  }
  return true;
}

function fingerprints(raw) {
  const out = new Set();
  for (const f of list(raw.fingerprint)) {
    if (f && f.modelID) out.add(`${f.manufacturerName || '*'}|${f.modelID}`);
  }
  for (const model of list(raw.zigbeeModel)) if (model) out.add(`*|${model}`);
  if (raw.model) out.add(`${raw.manufacturerName || '*'}|${raw.model}`);
  return [...out];
}

const aggregate = {
  definitions: 0,
  lights: 0,
  onoff: 0,
  dim: 0,
  dim_ct: 0,
  color: 0,
  template_candidates: 0,
  blockers: {},
  examples: [],
};

for (const file of fs.readdirSync(devicesDir).filter((f) => f.endsWith('.js'))) {
  let mod;
  try {
    mod = require(path.join(devicesDir, file));
  } catch {
    continue;
  }
  for (const raw of list(mod.definitions)) {
    aggregate.definitions += 1;
    const light = lightExpose(raw);
    if (!light) continue;
    aggregate.lights += 1;
    const names = features(light);
    const hasColor = ['color', 'color_hs', 'color_xy'].some((name) => names.has(name));
    const hasBrightness = names.has('brightness');
    const hasTemp = names.has('color_temp');
    const tier = hasColor ? 'color' : !hasBrightness ? 'onoff' : hasTemp ? 'dim_ct' : 'dim';
    aggregate[tier] += 1;

    const { from, to } = converters(raw);
    const bad = [];
    for (const converter of from) {
      if (!FZ_KNOWN.has(converter.convert)) bad.push(`from:${converter.cluster || '?'}`);
    }
    for (const converter of to) {
      if (!TZ_KNOWN.has(converter.convertSet || converter.convertGet)) {
        bad.push(`to:${(converter.key || ['?']).join(',')}`);
      }
    }
    if (!configureOk(raw)) bad.push('configure');
    if (tier === 'color') bad.push('capability:color');
    for (const reason of bad) aggregate.blockers[reason] = (aggregate.blockers[reason] || 0) + 1;
    if (tier !== 'color' && bad.length === 0) {
      aggregate.template_candidates += 1;
      if (aggregate.examples.length < 8) {
        aggregate.examples.push({
          model: raw.model,
          vendor: raw.vendor,
          tier,
          fingerprints: fingerprints(raw).slice(0, 3),
        });
      }
    }
  }
}

aggregate.top_blockers = Object.entries(aggregate.blockers)
  .sort((a, b) => b[1] - a[1])
  .slice(0, 20)
  .map(([reason, count]) => ({ reason, count }));

fs.writeFileSync(path.join(__dirname, 'lighting_report.json'), JSON.stringify(aggregate, null, 1));
console.log(JSON.stringify(aggregate, null, 2));
