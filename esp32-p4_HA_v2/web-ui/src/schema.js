// Схема записей v2 (docs/services/WEB_PROTOCOL.md §5) — зеркало ha_model.
// Только провод: дискриминаторы, layout key/record, декод и кодирование.

export const ENTITY = {
  DEVICE: 1, STATE: 2, ENDPOINT: 3, AUTOMATION: 4, DEVICE_REMOVE: 5, LOCATION: 6,
  GROUP: 7, GROUP_ITEM: 8, WEATHER: 9, WIFI_STATUS: 12, SETTINGS: 13,
}

export const SCHEMA = {
  [ENTITY.DEVICE]: { keySize: 8, recSize: 64 },
  [ENTITY.STATE]: { keySize: 16, recSize: 8 },
  [ENTITY.ENDPOINT]: { keySize: 16, recSize: 72 },
  [ENTITY.AUTOMATION]: { keySize: 8, recSize: 144 },
  [ENTITY.DEVICE_REMOVE]: { keySize: 8, recSize: 8 },
  [ENTITY.LOCATION]: { keySize: 8, recSize: 60 },
  [ENTITY.GROUP]: { keySize: 8, recSize: 32 },
  [ENTITY.GROUP_ITEM]: { keySize: 24, recSize: 36 },
  [ENTITY.WEATHER]: { keySize: 8, recSize: 16 },
  [ENTITY.WIFI_STATUS]: { keySize: 8, recSize: 36 },
  [ENTITY.SETTINGS]: { keySize: 1, recSize: 8 },
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
    case ENTITY.LOCATION:
    case ENTITY.WEATHER:
    case ENTITY.WIFI_STATUS:
      return { uid: dv.getBigUint64(0, true) }
    case ENTITY.GROUP:
      return { id: dv.getBigUint64(0, true) }
    case ENTITY.GROUP_ITEM:
      return {
        groupId: dv.getBigUint64(0, true),
        state: {
          uid: dv.getBigUint64(8, true),
          cluster: dv.getUint16(16, true),
          attr: dv.getUint16(18, true),
          ep: dv.getUint8(20),
        },
      }
    case ENTITY.SETTINGS:
      return { id: dv.getUint8(0) }
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
    case ENTITY.LOCATION:
      return {
        latitude: dv.getFloat32(0, true),
        longitude: dv.getFloat32(4, true),
        tzOffsetMin: dv.getInt16(8, true),
        name: cstr(dv, 12, 48),
      }
    case ENTITY.GROUP:
      return { title: cstr(dv, 0, 32) }
    case ENTITY.GROUP_ITEM:
      return { order: dv.getUint16(0, true), title: cstr(dv, 4, 32) }
    case ENTITY.WEATHER:
      return {
        condition: dv.getUint8(0),
        cloudPct: dv.getUint8(1),
        temperatureC: dv.getInt16(2, true) / 100,
        humidityPct: dv.getUint16(4, true) / 100,
        pressureHpa: dv.getUint16(6, true),
        windKmh: dv.getUint16(8, true) / 10,
        windDirDeg: dv.getUint16(10, true),
      }
    case ENTITY.WIFI_STATUS:
      return {
        state: dv.getUint8(0),
        connected: dv.getUint8(1),
        rssi: dv.getInt8(2),
        ssid: cstr(dv, 4, 32),
      }
    case ENTITY.SETTINGS:
      return {
        screensaverTimeoutMs: dv.getUint32(0, true),
        brightnessPct: dv.getUint8(4),
      }
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
        triggerKind: dv.getUint8(3),
        triggerMinutesOfDay: dv.getUint16(4, true),
        triggerWeekdayMask: dv.getUint8(6),
        /* STATE: порог @4 (f32) и device/ep/cluster/attr/op/edge @8..23. */
        triggerValue: dv.getFloat32(4, true),
        triggerEp: dv.getUint8(16),
        triggerCluster: dv.getUint16(18, true),
        triggerAttr: dv.getUint16(20, true),
        triggerOp: dv.getUint8(22),
        triggerEdge: dv.getUint8(23),
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
    case ENTITY.LOCATION:
      return `loc:${key.uid}`
    case ENTITY.WEATHER:
      return `weather:${key.uid}`
    case ENTITY.WIFI_STATUS:
      return `wifi:${key.uid}`
    case ENTITY.GROUP:
      return `grp:${key.id}`
    case ENTITY.GROUP_ITEM:
      return `gi:${key.groupId}:${key.state.uid}:${key.state.ep}:${key.state.cluster}:${key.state.attr}`
    case ENTITY.SETTINGS:
      return `set:${key.id}`
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
  const kind = r.triggerKind || 0
  dv.setUint8(0, r.enabled ? 1 : 0)
  dv.setUint8(1, Math.min(args.length, 8))
  dv.setUint8(2, conds.length)
  dv.setUint8(3, kind)
  dv.setBigUint64(8, BigInt(r.triggerUid || 0), true)
  if (kind === 2) {
    dv.setFloat32(4, Number(r.triggerValue) || 0, true)
    dv.setUint8(16, r.triggerEp || 0)
    dv.setUint16(18, r.triggerCluster || 0, true)
    dv.setUint16(20, r.triggerAttr || 0, true)
    dv.setUint8(22, r.triggerOp || 1)
    dv.setUint8(23, r.triggerEdge || 0)
  } else {
    dv.setUint16(4, kind === 1 ? (r.triggerMinutesOfDay || 0) : 0, true)
    dv.setUint8(6, kind === 1 ? (r.triggerWeekdayMask ?? 0x7f) : 0)
    dv.setUint16(16, r.triggerCmd || 0, true)
  }
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
