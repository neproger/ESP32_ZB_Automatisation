// Дамп определений zigbee-herdsman-converters в TSV (без JSON).
// Usage: node dump_z2m.mjs <path/to/node_modules/zigbee-herdsman-converters/dist/devices>
// Поля: manufacturer, model, vendor, exposes-сигнатура, flags, description.
import { createRequire } from 'node:module'
import { readdirSync } from 'node:fs'
import { join } from 'node:path'

const require = createRequire(import.meta.url)
const root = process.argv[2]
if (!root) {
  console.error('usage: node dump_z2m.mjs <devices-dir>')
  process.exit(2)
}

function flatten(exposes, out) {
  if (!Array.isArray(exposes)) return
  for (const e of exposes) {
    if (!e || typeof e !== 'object') continue
    if (e.type && e.name !== undefined) {
      const values = Array.isArray(e.values) ? e.values.join('/') : ''
      out.push([e.type, e.name || e.property || '', e.property || '', e.unit || '', values].join('|'))
    }
    if (Array.isArray(e.features)) flatten(e.features, out)
  }
}

for (const file of readdirSync(root).filter((f) => f.endsWith('.js'))) {
  let mod
  try {
    mod = require(join(root, file))
  } catch {
    continue
  }
  for (const d of mod.definitions || []) {
    const fingerprints = (d.fingerprint || []).filter((f) => f.manufacturerName && f.modelID)
    if (fingerprints.length === 0) continue
    const out = []
    flatten(d.exposes, out)
    const tuyaDp = (d.fromZigbee || []).some(
      (c) => c && String(c.cluster).toLowerCase() === 'manuspecifictuya',
    )
    const flags = tuyaDp ? 'tuya_dp' : ''
    const description = String(d.description || '').replace(/[\t\n\r]/g, ' ')
    for (const fp of fingerprints) {
      console.log(
        [fp.manufacturerName, fp.modelID, d.vendor || '', out.join(';'), flags, description].join('\t'),
      )
    }
  }
}
