// Схема записей v2 — зеркало ha_model (docs/services/WEB_PROTOCOL.md §5).
// Смещения/размеры совпадают с layout'ом структур C (включая выравнивание).

export const ENTITY = { DEVICE: 1, STATE: 2, ENDPOINT: 3, AUTOMATION: 4 }

export const SCHEMA = {
  [ENTITY.DEVICE]: { keySize: 8, recSize: 64 },
  [ENTITY.STATE]: { keySize: 16, recSize: 8 },
  [ENTITY.ENDPOINT]: { keySize: 16, recSize: 72 },
  [ENTITY.AUTOMATION]: { keySize: 8, recSize: 48 },
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
    case ENTITY.AUTOMATION:
      return {
        enabled: dv.getUint8(0),
        triggerUid: dv.getBigUint64(8, true),
        triggerCmd: dv.getUint16(16, true),
        actionUid: dv.getBigUint64(24, true),
        actionEp: dv.getUint8(32),
        actionCluster: dv.getUint16(34, true),
        actionCmd: dv.getUint8(36),
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
    default:
      return ''
  }
}

const ATTR_NAMES = {
  '6:0': 'On/Off',
  '8:0': 'Уровень',
  '1024:0': 'Освещённость',
  '1026:0': 'Температура',
  '1029:0': 'Влажность',
  '1030:0': 'Давление',
  '1024:0x0010': 'Освещённость',
  '1026:0x0010': 'Температура',
}

export function attrName(cluster, attr) {
  return ATTR_NAMES[`${cluster}:${attr}`] || `cluster ${cluster}/${attr}`
}

// Значение атрибута: ZCL-тип + сырые байты. Масштаб — сторона читателя (ha_zigbee.h).
export function formatValue(cluster, attr, zclType, raw) {
  const u = raw >>> 0
  switch (zclType) {
    case 0x10:
      return u ? 'Вкл' : 'Выкл'
    case 0x28:
      return String((u << 24) >> 24)
    case 0x29:
      return (cluster === 0x0402 ? ((u << 16) >> 16) / 100 : String((u << 16) >> 16))
    case 0x2b:
      return String(u | 0)
    case 0x20:
      return String(u & 0xff)
    case 0x21:
      return String(u & 0xffff)
    case 0x18:
      return '0x' + (u & 0xff).toString(16)
    default:
      return String(u)
  }
}

export const CLUSTER_ONOFF = 6
export const CLUSTER_LEVEL = 8
export const CMD_ONOFF = { OFF: 0, ON: 1, TOGGLE: 2 }
export const CMD_LEVEL_MOVE_TO = 0
