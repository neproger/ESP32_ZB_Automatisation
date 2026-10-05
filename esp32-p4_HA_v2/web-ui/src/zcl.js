// ZCL-справочник: имена кластеров/атрибутов, единицы и масштаб (порт v1 zcl.js).
// Значения приходят сырыми (u32 + zcl_type); сюда же — форматирование для UI.

export function hex16(v) {
  return '0x' + ((Number(v) || 0) >>> 0).toString(16).padStart(4, '0')
}

export function uidHex(uid) {
  return '0x' + uid.toString(16).padStart(16, '0')
}

export const CLUSTER_ONOFF = 0x0006
export const CLUSTER_LEVEL = 0x0008
export const CMD_ONOFF = { OFF: 0, ON: 1, TOGGLE: 2 }
export const CMD_LEVEL_MOVE_TO = 0x00

const ATTR_NAMES = {
  '0:4': 'производитель',
  '0:5': 'модель',
  '6:0': 'состояние',
  '8:0': 'уровень',
}

export function attrName(cluster, attr) {
  const info = describeAttr(cluster, attr)
  if (info) return info.name
  return ATTR_NAMES[`${cluster}:${attr}`] || 'attr ' + hex16(attr)
}

const CLUSTER_NAMES = {
  0x0000: 'Basic',
  0x0001: 'Питание',
  0x0003: 'Identify',
  0x0004: 'Группы',
  0x0005: 'Сцены',
  0x0006: 'On/Off',
  0x0007: 'On/Off Switch',
  0x0008: 'Level Control',
  0x000a: 'Time',
  0x0019: 'OTA',
  0x0102: 'Шторы',
  0x0201: 'Термостат',
  0x0202: 'Вентилятор',
  0x0300: 'Color Control',
  0x0400: 'Illuminance',
  0x0402: 'Температура',
  0x0403: 'Давление',
  0x0405: 'Влажность',
  0x0406: 'Присутствие',
  0x0500: 'IAS Zone',
  0x0702: 'Metering',
  0x0b04: 'Электроизмерения',
  0xfc00: 'Время',
}

export function clusterName(id) {
  return CLUSTER_NAMES[id] || hex16(id)
}

const DEVICE_IDS = {
  0x0000: 'On/Off Light',
  0x0100: 'On/Off Plug',
  0x0101: 'Dimmable Light',
  0x0302: 'Датчик температуры',
  0x0402: 'Датчик света',
  0x0405: 'Датчик влажности',
}

// Проекция (cluster, attr) → виджет Display (зеркало ui_widget_kind_for).
export function widgetKind(cluster, attr) {
  if (cluster === 0x0006 && attr === 0x0000) return 'Переключатель'
  if (cluster === 0x0008 && attr === 0x0000) return 'Уровень'
  if (cluster === 0x0300) return attr === 0x0007 ? 'Темп. цвета' : 'Цвет'
  if (cluster === 0x0406 && attr === 0x0000) return 'Индикатор'
  if (cluster === 0x0500 && attr === 0x0000) return 'Индикатор'
  return 'Значение'
}

export function describeProfile(id) {
  return id === 0x0104 ? 'Home Automation' : null
}
export function describeDeviceId(id) {
  return DEVICE_IDS[id] || null
}

// name + unit + scale + signed для известных атрибутов.
export function describeAttr(cluster, attr) {
  if (cluster === 0x0006 && attr === 0x0000) return { name: 'OnOff' }
  if (cluster === 0x0008 && attr === 0x0000) return { name: 'CurrentLevel' }
  if (cluster === 0x0300 && attr === 0x0003) return { name: 'CurrentX' }
  if (cluster === 0x0300 && attr === 0x0004) return { name: 'CurrentY' }
  if (cluster === 0x0300 && attr === 0x0007) return { name: 'ColorTemperature', unit: 'mired' }
  if (cluster === 0x0402 && attr === 0x0000) return { name: 'MeasuredValue', unit: '°C', scale: 0.01, signed: true }
  if (cluster === 0x0405 && attr === 0x0000) return { name: 'MeasuredValue', unit: '%', scale: 0.01 }
  if (cluster === 0x0400 && attr === 0x0000) return { name: 'MeasuredValue' }
  if (cluster === 0x0001 && attr === 0x0021) return { name: 'Battery', unit: '%', scale: 0.5 }
  if (cluster === 0x0001 && attr === 0x0020) return { name: 'BatteryVoltage', unit: 'V', scale: 0.1 }
  if (cluster === 0x000a && attr === 0x0000) return { name: 'Время (UTC)' }
  if (cluster === 0xfc00) return SYSTEM_ATTR[attr] || null
  return null
}

// Атрибуты системного девайса (см. ha_model/ha_system.h).
const SYSTEM_ATTR = {
  0x0000: { name: 'Час' },
  0x0001: { name: 'Минута' },
  0x0002: { name: 'День недели' },
  0x0003: { name: 'Дни недели (маска)' },
  0x0004: { name: 'Минуты от начала суток' },
  0x0005: { name: 'Смещение пояса', unit: 'мин' },
}

const WEEKDAYS = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс']

function formatSystemValue(attr, raw) {
  const u = raw >>> 0
  if (attr === 0x0000) return `${u & 0xff}`
  if (attr === 0x0001) return String(u & 0xff).padStart(2, '0')
  if (attr === 0x0002) return WEEKDAYS[u & 7] || '?'
  if (attr === 0x0003) return WEEKDAYS.filter((_, i) => (u >> i) & 1).join(', ') || '—'
  if (attr === 0x0004) return `${String(Math.floor(u / 60)).padStart(2, '0')}:${String(u % 60).padStart(2, '0')}`
  if (attr === 0x0005) {
    const v = (u << 16) >> 16
    const sign = v < 0 ? '-' : '+'
    const a = Math.abs(v)
    return `UTC${sign}${String(Math.floor(a / 60)).padStart(2, '0')}:${String(a % 60).padStart(2, '0')}`
  }
  return String(u)
}

function signExtend(u, zclType) {
  if (zclType === 0x28) return (u << 24) >> 24 // int8
  if (zclType === 0x29) return (u << 16) >> 16 // int16
  if (zclType === 0x2b) return u | 0 // int32
  return u
}

export function formatAttrValue(cluster, attr, zclType, raw) {
  const u = raw >>> 0
  if (cluster === 0xfc00) return formatSystemValue(attr, raw)
  if (cluster === 0x000a && attr === 0x0000) return new Date(u * 1000).toLocaleString()
  const info = describeAttr(cluster, attr)
  if (info?.scale) {
    const v = (info.signed ? signExtend(u, zclType) : u) * info.scale
    const digits = info.scale < 1 ? Math.min(3, Math.max(0, Math.round(-Math.log10(info.scale)))) : 0
    return `${v.toFixed(digits)}${info.unit ? ' ' + info.unit : ''}`
  }
  if (zclType === 0x10) return u ? 'Вкл' : 'Выкл'
  if (zclType === 0x20) return String(u & 0xff)
  if (zclType === 0x21) return String(u & 0xffff)
  if (info?.unit) return `${u} ${info.unit}`
  return String(u)
}
