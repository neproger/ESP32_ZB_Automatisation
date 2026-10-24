// Instrumented Z2M catalog spike: records modernExtend provenance, coverage and
// unsupported rules for a fixed set of models. Dev tool only.
const fs = require('node:fs');
const path = require('node:path');

const pkg = process.argv[2];
const dir = process.argv[3];
if (!pkg || !dir) {
  console.error('usage: node generate.cjs <zhc-pkg> <dist/devices>');
  process.exit(2);
}

const calls = [];
const stack = [];

function clean(v, depth = 0) {
  if (depth > 2) return '...';
  if (typeof v === 'function') return '[fn]';
  if (Array.isArray(v)) return v.slice(0, 8).map((x) => clean(x, depth + 1));
  if (v && typeof v === 'object') {
    const out = {};
    for (const k of Object.keys(v).slice(0, 12)) out[k] = clean(v[k], depth + 1);
    return out;
  }
  return v;
}

function wrap(obj, ns) {
  for (const key of Object.keys(obj)) {
    const fn = obj[key];
    if (typeof fn !== 'function') continue;
    obj[key] = function (...args) {
      const record = { helper: ns + '.' + key, args: clean(args.length === 1 ? args[0] : args), children: [] };
      calls.push(record);
      if (stack.length) stack[stack.length - 1].children.push(record);
      stack.push(record);
      try {
        const result = fn.apply(this, args);
        record.result = result;
        return result;
      } finally {
        stack.pop();
      }
    };
  }
}

wrap(require(path.join(pkg, 'dist/lib/tuya')).modernExtend, 'tuya');
wrap(require(path.join(pkg, 'dist/lib/modernExtend')), 'modernExtend');

const targets = [
  { label: 'TS011F', model: 'TS011F' },
  { label: 'SNZB-01P', model: 'SNZB-01P' },
  { label: 'WXKG11LM', model: 'WXKG11LM' },
];
const found = new Map();

for (const file of fs.readdirSync(dir).filter((f) => f.endsWith('.js'))) {
  let mod;
  try {
    mod = require(path.join(dir, file));
  } catch {
    continue;
  }
  for (const raw of mod.definitions || []) {
    const models = [raw.model, ...(raw.zigbeeModel || []), ...(raw.fingerprint || []).map((f) => f.modelID)].filter(Boolean);
    const hit = targets.find((t) => models.includes(t.model));
    if (!hit) continue;
    const current = found.get(hit.label);
    if (current && current.raw.model === hit.model) continue; // exact model beats alternates
    found.set(hit.label, { file, raw });
  }
}

function configureType(value) {
  if (Array.isArray(value)) return 'functions:' + value.length;
  return typeof value;
}

function describe(def) {
  if (!def) return null;
  const steps = Array.isArray(def.configure) ? def.configure.map((s) => clean(s)) : [];
  const exposes = (def.exposes || []).map((e) => ({ type: e.type, name: e.name, property: e.property, values: Array.isArray(e.values) ? e.values.map((v) => v.name ?? v) : undefined }));
  const from = (def.fromZigbee || []).map((c) => ({ cluster: String(c.cluster), type: c.type, attribute: c.attribute, command: c.command, dp: c.dp }));
  const to = (def.toZigbee || []).map((c) => ({ key: c.key, cluster: c.cluster, type: c.type, dp: c.dp }));
  return { steps, exposes, from, to };
}

function strip(record) {
  const def = record.result;
  return {
    helper: record.helper,
    args: record.args,
    children: record.children.map(strip),
    made: {
      exposes: (def && def.exposes ? def.exposes.length : 0) || 0,
      from: (def && def.fromZigbee ? def.fromZigbee.length : 0) || 0,
      to: (def && def.toZigbee ? def.toZigbee.length : 0) || 0,
      configure: configureType(def && def.configure),
    },
    detail: describe(def),
  };
}

const report = { zhc: path.basename(pkg), devices: [] };
for (const target of targets) {
  const entry = found.get(target.label);
  if (!entry) {
    report.devices.push({ target: target.label, identified: false });
    continue;
  }
  const { file, raw } = entry;
  const ext = raw.extend || raw.extends || [];
  const provenance = ext.map((e) => {
    const call = calls.find((c) => c.result === e);
    if (call) return strip(call);
    return {
      helper: 'inline',
      args: null,
      children: [],
      made: {
        exposes: (e.exposes || []).length,
        from: (e.fromZigbee || []).length,
        to: (e.toZigbee || []).length,
        configure: configureType(e.configure),
      },
      inlineKeys: Object.keys(e).slice(0, 8),
      detail: describe(e),
    };
  });
  const fromData = provenance.reduce((n, p) => n + p.made.from, 0);
  const toData = provenance.reduce((n, p) => n + p.made.to, 0);
  const manualFrom = (raw.fromZigbee || []).length;
  const manualTo = (raw.toZigbee || []).length;
  report.devices.push({
    target: target.label,
    model: raw.model,
    vendor: raw.vendor,
    file,
    identified: true,
    states: {
      identified: true,
      decoded: fromData + manualFrom > 0,
      controlled: toData + manualTo > 0,
      configure: typeof raw.configure === 'function' ? 'manual' : 'fragments',
    },
    counts: { manualFrom, manualTo, fragmentFrom: fromData, fragmentTo: toData, exposes: (raw.exposes || []).length },
    provenance,
    manual: {
      from: (raw.fromZigbee || []).map((c) => ({ cluster: String(c.cluster), type: c.type })),
      to: (raw.toZigbee || []).map((c) => ({ key: c.key, description: c.description })),
    },
  });
}

fs.writeFileSync(path.join(__dirname, 'report.json'), JSON.stringify(report, null, 2));

for (const d of report.devices) {
  if (!d.identified) {
    console.log('@ ' + d.target + ' NOT IDENTIFIED');
    continue;
  }
  console.log('@ ' + d.target + ' model=' + d.model + ' vendor=' + d.vendor);
  console.log(
    '  decoded=' + d.states.decoded + ' controlled=' + d.states.controlled + ' configure=' + d.states.configure,
  );
  console.log(
    '  fragments: from=' + d.counts.fragmentFrom + ' to=' + d.counts.fragmentTo +
      '; manual: from=' + d.counts.manualFrom + ' to=' + d.counts.manualTo,
  );
  for (const p of d.provenance) {
    console.log(
      '  - ' + p.helper + ' ' + JSON.stringify(p.args) +
        ' -> exposes=' + p.made.exposes + ' from=' + p.made.from + ' to=' + p.made.to +
        ' configure=' + p.made.configure,
    );
  }
  for (const f of d.manual.from) {
    console.log('  manual from cluster=' + f.cluster + ' type=' + JSON.stringify(f.type));
  }
}
console.log('report: tools/z2m_spike/report.json');
