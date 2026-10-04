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
    case ENTITY.AUTOMATION: {
      const argsLen = dv.getUint8(1)
      const actionArgs = []
      for (let i = 0; i < argsLen && i < 8; i++) actionArgs.push(dv.getUint8(37 + i))
      return {
        enabled: dv.getUint8(0),
        triggerUid: dv.getBigUint64(8, true),
        triggerCmd: dv.getUint16(16, true),
        actionUid: dv.getBigUint64(24, true),
        actionEp: dv.getUint8(32),
        actionCluster: dv.getUint16(34, true),
        actionCmd: dv.getUint8(36),
        actionArgs,
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
    default:
      return ''
  }
}

const ATTR_NAMES = {
  '0:4': 'производитель',
  '0:5': 'модель',
  '6:0': 'состояние',
  '8:0': 'уровень',
  '1024:0': 'значение',
  '1026:0': 'значение',
  '1029:0': 'значение',
  '1030:0': 'значение',
}

export function attrName(cluster, attr) {
  return ATTR_NAMES[`${cluster}:${attr}`] || 'attr 0x' + attr.toString(16)
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

const CLUSTER_NAMES = {
  0x0000: 'Basic',
  0x0001: 'Питание',
  0x0003: 'Identify',
  0x0004: 'Группы',
  0x0005: 'Сцены',
  0x0006: 'On/Off',
  0x0007: 'On/Off Switch',
  0x0008: 'Уровень',
  0x000a: 'Analog Input',
  0x0019: 'OTA',
  0x0102: 'Шторы',
  0x0201: 'Термостат',
  0x0202: 'Вентилятор',
  0x0300: 'Цвет',
  0x0400: 'Освещённость',
  0x0402: 'Температура',
  0x0403: 'Давление',
  0x0405: 'Влажность',
  0x0406: 'Присутствие',
  0x0500: 'IAS Zone',
  0x0702: 'Metering',
  0x0b04: 'Электроизмерения',
}

export function clusterName(id) {
  return CLUSTER_NAMES[id] || 'cluster 0x' + id.toString(16)
}

export function uidHex(uid) {
  return '0x' + uid.toString(16).padStart(16, '0')
}

// Кодирование записи правила (48 байт, layout C с выравниванием; см. WEB_PROTOCOL §5).
export function encodeAutomationRecord(r) {
  const out = new Uint8Array(48)
  const dv = new DataView(out.buffer)
  const args = r.actionArgs || []
  dv.setUint8(0, r.enabled ? 1 : 0)
  dv.setUint8(1, Math.min(args.length, 8))
  dv.setBigUint64(8, BigInt(r.triggerUid || 0), true)
  dv.setUint16(16, r.triggerCmd || 0, true)
  dv.setBigUint64(24, BigInt(r.actionUid || 0), true)
  dv.setUint8(32, r.actionEp || 0)
  dv.setUint16(34, r.actionCluster || 0, true)
  dv.setUint8(36, r.actionCmd || 0)
  out.set(args.slice(0, 8), 37)
  return out
}
