const fs = require('node:fs');
const path = require('node:path');
const zhc = require(process.argv[2]);
const dir = process.argv[3];
const targets = ['TS011F', 'WXKG11LM', 'SNZB-01P'];
const found = new Map();

for (const file of fs.readdirSync(dir).filter((f) => f.endsWith('.js'))) {
  let mod;
  try { mod = require(path.join(dir, file)); } catch { continue; }
  for (const raw of mod.definitions || []) {
    const models = [raw.model, ...(raw.zigbeeModel || []), ...(raw.fingerprint || []).map((f) => f.modelID)].filter(Boolean);
    const hit = targets.find((t) => models.includes(t));
    if (!hit) continue;
    const cur = found.get(hit);
    if (cur && cur.raw.model === hit) continue;
    found.set(hit, { file, raw });
  }
}

for (const target of targets) {
  const entry = found.get(target);
  if (!entry) { console.log(`@ ${target} missing`); continue; }
  const { file, raw } = entry;
  const ext = raw.extend || raw.extends || [];
  console.log(`@ ${target} model=${raw.model} (${raw.vendor}) file=${file}`);
  console.log(`  zigbeeModel=${JSON.stringify(raw.zigbeeModel || [])} fingerprint=${(raw.fingerprint || []).length}`);
  console.log(`  raw: exposes=${(raw.exposes || []).length} from=${(raw.fromZigbee || []).length} to=${(raw.toZigbee || []).length} extend=${ext.length} configure=${typeof raw.configure} onEvent=${typeof raw.onEvent}`);
  ext.forEach((e, i) => {
    const keys = Object.keys(e || {}).filter((k) => k !== 'meta').slice(0, 8);
    console.log(`  extend[${i}] keys=${keys.join(',')} exposes=${(e.exposes || []).length} from=${(e.fromZigbee || []).length}`);
  });
  (raw.fromZigbee || []).forEach((c, i) => console.log(`  from[${i}] cluster=${String(c.cluster)} type=${JSON.stringify(c.type)} keys=${Object.keys(c).filter((k) => k !== 'convert').slice(0, 6).join(',')}`));
}
