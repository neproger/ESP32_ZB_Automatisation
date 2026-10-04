// Схема записей v2 (docs/services/WEB_PROTOCOL.md §5) — зеркало ha_model.
// Только провод: дискриминаторы, layout key/record, декод и кодирование.

export const ENTITY = { DEVICE: 1, STATE: 2, ENDPOINT: 3, AUTOMATION: 4, DEVICE_REMOVE: 5 }

export const SCHEMA = {
  [ENTITY.DEVICE]: { keySize: 8, recSize: 64 },
  [ENTITY.STATE]: { keySize: 16, recSize: 8 },
  [ENTITY.ENDPOINT]: { keySize: 16, recSize: 72 },
  [ENTITY.AUTOMATION]: { keySize: 8, recSize: 144 },
  [ENTITY.DEVICE_REMOVE]: { keySize: 8, recSize: 8 },
}

const text = new TextDecoder()

function cstr(dv, off, len) {
  const bytes = new Uint8Array(dv.buffer, dv.byteOffset + off, len)
  const end = bytes.indexOf(0)
  return text.decode(end === -1 ? bytes : bytes.subarray(0, end))
}

export function decodeKey(type, dv) {
  switch (type) {
    case ENTITY.DEVICE:
      return { uid: dv.getBigUint64(0, true) }
    case ENTITY.STATE:
      return {
        uid: dv.getBigUint64(0, true),
        cluster: dv.getUint16(8, true),
        attr: dv.getUint16(10, true),
        ep: dv.getUint8(12),
      }
    case ENTITY.ENDPOINT:
      return { uid: dv.getBigUint64(0, true), ep: dv.getUint8(8) }
    case ENTITY.AUTOMATION:
      return { id: dv.getBigUint64(0, true) }
    case ENTITY.DEVICE_REMOVE:
      return { uid: dv.getBigUint64(0, true) }
    default:
      return {}
  }
}

export function decodeRecord(type, dv) {
  switch (type) {
    case ENTITY.DEVICE:
      return { name: cstr(dv, 0, 32), model: cstr(dv, 32, 32) }
    case ENTITY.STATE:
      return { raw: dv.getUint32(0, true), zclType: dv.getUint8(4) }
    case ENTITY.ENDPOINT: {
      const count = Math.min(dv.getUint8(4), 16)
      const clusters = []
      for (let i = 0; i < count; i++) {
        const o = 8 + i * 4
        clusters.push({ id: dv.getUint16(o, true), role: dv.getUint8(o + 2) })
      }
      return { profile: dv.getUint16(0, true), deviceId: dv.getUint16(2, true), clusters }
    }
    case ENTITY.DEVICE_REMOVE:
      return { requested: dv.getUint8(0) }
    case ENTITY.AUTOMATION: {
      const argsLen = dv.getUint8(1)
      const actionArgs = []
      for (let i = 0; i < argsLen && i < 8; i++) actionArgs.push(dv.getUint8(37 + i))
      const conditionsCount = Math.min(dv.getUint8(2), 4)
      const conditions = []
      for (let i = 0; i < conditionsCount; i++) {
        const o = 48 + i * 24
        conditions.push({
          deviceUid: dv.getBigUint64(o, true),
          cluster: dv.getUint16(o + 8, true),
          attr: dv.getUint16(o + 10, true),
          ep: dv.getUint8(o + 12),
          op: dv.getUint8(o + 13),
          value: dv.getFloat32(o + 16, true),
        })
      }
      return {
        enabled: dv.getUint8(0),
        triggerUid: dv.getBigUint64(8, true),
        triggerCmd: dv.getUint16(16, true),
        actionUid: dv.getBigUint64(24, true),
        actionEp: dv.getUint8(32),
        actionCluster: dv.getUint16(34, true),
        actionCmd: dv.getUint8(36),
        actionArgs,
        conditions,
      }
    }
    default:
      return {}
  }
}

export function entityId(type, key) {
  switch (type) {
    case ENTITY.DEVICE:
      return `dev:${key.uid}`
    case ENTITY.STATE:
      return `st:${key.uid}:${key.ep}:${key.cluster}:${key.attr}`
    case ENTITY.ENDPOINT:
      return `ep:${key.uid}:${key.ep}`
    case ENTITY.AUTOMATION:
      return `auto:${key.id}`
    case ENTITY.DEVICE_REMOVE:
      return `rm:${key.uid}`
    default:
      return ''
  }
}

// Кодирование записи правила (144 байта, layout C с выравниванием; WEB_PROTOCOL §5).
export function encodeAutomationRecord(r) {
  const out = new Uint8Array(144)
  const dv = new DataView(out.buffer)
  const args = r.actionArgs || []
  const conds = (r.conditions || []).slice(0, 4)
  dv.setUint8(0, r.enabled ? 1 : 0)
  dv.setUint8(1, Math.min(args.length, 8))
  dv.setUint8(2, conds.length)
  dv.setBigUint64(8, BigInt(r.triggerUid || 0), true)
  dv.setUint16(16, r.triggerCmd || 0, true)
  dv.setBigUint64(24, BigInt(r.actionUid || 0), true)
  dv.setUint8(32, r.actionEp || 0)
  dv.setUint16(34, r.actionCluster || 0, true)
  dv.setUint8(36, r.actionCmd || 0)
  out.set(args.slice(0, 8), 37)
  conds.forEach((c, i) => {
    const o = 48 + i * 24
    dv.setBigUint64(o, BigInt(c.deviceUid || 0), true)
    dv.setUint16(o + 8, c.cluster || 0, true)
    dv.setUint16(o + 10, c.attr || 0, true)
    dv.setUint8(o + 12, c.ep || 0)
    dv.setUint8(o + 13, c.op || 1)
    dv.setFloat32(o + 16, Number(c.value) || 0, true)
  })
  return out
}
