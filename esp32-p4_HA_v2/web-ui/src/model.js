import { ENTITY, MSG, SIZES } from './proto.js'
import { VALUE_KIND } from './semantics.js'
import { ENTITY_SYSTEM } from './system.js'
import { pushEvent } from './events.js'

// Canonical-модель браузера: Maps по типам + разбор canonical records (verbatim layout
// ha_model). Здесь только хранение и декод; транспорт, синхронизация и «версия» — в store.js.

export const locations = new Map()
export const weatherMap = new Map()
export const wifiStatusMap = new Map()
export const settingsMap = new Map()
export const groups = new Map()
export const semAutomations = new Map()
export const semGroupItems = new Map()
export const semGroupAutomations = new Map()
export const semEntities = new Map()
export const semDevices = new Map()
export const propStates = new Map()
export const propCapabilities = new Map()
export const propEvents = new Map()

const dec = new TextDecoder()
function cstr(dv, off, len) {
  const bytes = new Uint8Array(dv.buffer, dv.byteOffset + off, len)
  const end = bytes.indexOf(0)
  return dec.decode(end === -1 ? bytes : bytes.subarray(0, end))
}
const textAt = (dv, off, len) => cstr(dv, off, len)

function decodeLocation(dv, o) {
  const flags = dv.getUint8(o + 10)
  return {
    latitude: dv.getFloat32(o, true), longitude: dv.getFloat32(o + 4, true),
    tzOffsetMin: dv.getInt16(o + 8, true), tzAuto: (flags & 1) !== 0, posAuto: (flags & 2) !== 0,
    name: cstr(dv, o + 12, 48),
  }
}
function decodeWeather(dv, o) {
  return {
    condition: dv.getUint8(o), cloudPct: dv.getUint8(o + 1),
    temperatureC: dv.getInt16(o + 2, true) / 100, humidityPct: dv.getUint16(o + 4, true) / 100,
    pressureHpa: dv.getUint16(o + 6, true), windKmh: dv.getUint16(o + 8, true) / 10,
    windDirDeg: dv.getUint16(o + 10, true),
  }
}
function decodeWifiStatus(dv, o) {
  return { state: dv.getUint8(o), connected: dv.getUint8(o + 1), rssi: dv.getInt8(o + 2), ssid: cstr(dv, o + 4, 32) }
}
function decodeSettings(dv, o) {
  return { screensaverTimeoutMs: dv.getUint32(o, true), brightnessPct: dv.getUint8(o + 4) }
}
const decodeGroup = (dv, o) => ({ title: cstr(dv, o, 32) })

const CONDITION_SIZE = 32
const RULE_TRIGGERS_OFF = 0
const RULE_ACTIONS_OFF = 128
const RULE_CONDITIONS_OFF = 256
const RULE_TRIGGER_SIZE = 32
const RULE_ACTION_SIZE = 32
const RULE_OFF_ENABLED = 384
const RULE_OFF_TRIGGER_COUNT = 385
const RULE_OFF_ACTION_COUNT = 386
const RULE_OFF_CONDITIONS_COUNT = 387
const RULE_OFF_NAME = 392

function decodeTrigger(dv, o) {
  return {
    kind: dv.getUint8(o + 26),
    entity: dv.getBigUint64(o, true),
    eventId: dv.getInt32(o + 8, true),
    property: dv.getInt32(o + 12, true),
    value: dv.getFloat32(o + 16, true),
    value2: dv.getFloat32(o + 20, true),
    minutesOfDay: dv.getUint16(o + 24, true),
    op: dv.getUint8(o + 27),
    edge: dv.getUint8(o + 28),
    weekdayMask: dv.getUint8(o + 29),
  }
}

function decodeAction(dv, o) {
  const a = {
    entity: dv.getBigUint64(o, true),
    property: dv.getInt32(o + 8, true),
    action: dv.getUint8(o + 28),
    valueKind: dv.getInt32(o + 16, true),
  }
  if (a.valueKind === 1) {
    a.scalarKind = dv.getInt32(o + 20, true)
    a.value = a.scalarKind === 4 ? dv.getFloat32(o + 24, true) : dv.getUint32(o + 24, true)
  } else if (a.valueKind === 2) {
    a.x = dv.getFloat32(o + 20, true)
    a.y = dv.getFloat32(o + 24, true)
  }
  return a
}

function decodeRule(dv, base) {
  const triggerCount = dv.getUint8(base + RULE_OFF_TRIGGER_COUNT)
  const actionCount = dv.getUint8(base + RULE_OFF_ACTION_COUNT)
  const conditionsCount = dv.getUint8(base + RULE_OFF_CONDITIONS_COUNT)
  const triggers = []
  for (let i = 0; i < triggerCount; i++) {
    triggers.push(decodeTrigger(dv, base + RULE_TRIGGERS_OFF + i * RULE_TRIGGER_SIZE))
  }
  const actions = []
  for (let i = 0; i < actionCount; i++) {
    actions.push(decodeAction(dv, base + RULE_ACTIONS_OFF + i * RULE_ACTION_SIZE))
  }
  const conditions = []
  for (let i = 0; i < conditionsCount; i++) {
    const p = base + RULE_CONDITIONS_OFF + i * CONDITION_SIZE
    conditions.push({
      entity: dv.getBigUint64(p, true),
      property: dv.getInt32(p + 8, true),
      op: dv.getUint8(p + 16),
      value: dv.getFloat32(p + 20, true),
      value2: dv.getFloat32(p + 24, true),
    })
  }
  return { enabled: dv.getUint8(base + RULE_OFF_ENABLED) !== 0, name: cstr(dv, base + RULE_OFF_NAME, 64), triggers, actions, conditions }
}

function decodeValue(payload, atKind, atBits) {
  const kind = payload.getUint8(atKind)
  let value = null
  switch (kind) {
    case VALUE_KIND.BOOL: value = payload.getUint32(atBits, true) !== 0; break
    case VALUE_KIND.I32: value = payload.getInt32(atBits, true); break
    case VALUE_KIND.U32:
    case VALUE_KIND.ENUM: value = payload.getUint32(atBits, true); break
    case VALUE_KIND.FLOAT: value = payload.getFloat32(atBits, true); break
    default: value = null
  }
  return { kind, value }
}

export function clearModel() {
  for (const m of [semDevices, semEntities, propCapabilities, semAutomations, semGroupItems, semGroupAutomations, groups, locations, settingsMap]) m.clear()
}
export function clearState() {
  for (const m of [propStates, weatherMap, wifiStatusMap]) m.clear()
}
export function clearAll() {
  clearModel()
  clearState()
}

// Применить один canonical record [key][record] по типу (без emit).
export function applyEntity(type, p) {
  if (type === ENTITY.LOGICAL_DEVICE) {
    const id = p.getBigUint64(0, true)
    semDevices.set(id.toString(), { id, parentId: p.getBigUint64(8, true), name: textAt(p, 52, 32) })
  } else if (type === ENTITY.ENTITY) {
    const id = p.getBigUint64(0, true)
    semEntities.set(id.toString(), { id, deviceId: p.getBigUint64(8, true), name: textAt(p, 16, 32) })
  } else if (type === ENTITY.PROPERTY_STATE) {
    const entity = p.getBigUint64(0, true)
    const property = p.getInt32(8, true)
    const key = `${entity}:${property}`
    const prev = propStates.get(key)
    const { kind, value } = decodeValue(p, 16, 20)
    propStates.set(key, { entity, property, kind, value })
    if (entity !== ENTITY_SYSTEM && (!prev || prev.value !== value)) {
      pushEvent({ kind: 'state', entity, property, valueKind: kind, value })
    }
  } else if (type === ENTITY.CAPABILITIES) {
    const key = p.getBigUint64(0, true).toString()
    const count = p.getUint8(8)
    const caps = []
    for (let i = 0; i < count; i++) {
      const property = p.getInt32(12 + i * 24, true)
      const ac = p.getUint8(16 + i * 24)
      const actions = []
      for (let j = 0; j < ac; j++) actions.push(p.getInt32(20 + i * 24 + j * 4, true))
      caps.push({ property, actions })
    }
    const ec = p.getUint8(204)
    const evs = []
    for (let i = 0; i < ec; i++) evs.push(p.getInt32(208 + i * 4, true))
    propCapabilities.set(key, caps)
    propEvents.set(key, evs)
  } else if (type === ENTITY.AUTOMATION) {
    const id = p.getBigUint64(0, true)
    semAutomations.set(id.toString(), { id, rule: decodeRule(p, 8) })
  } else if (type === ENTITY.GROUP_ITEM) {
    const groupId = p.getBigUint64(0, true)
    const entity = p.getBigUint64(8, true)
    semGroupItems.set(`${groupId}:${entity}`, {
      groupId, entity, order: p.getUint16(16, true), title: textAt(p, 20, 32),
    })
  } else if (type === ENTITY.GROUP_AUTOMATION) {
    const groupId = p.getBigUint64(0, true)
    const automationId = p.getBigUint64(8, true)
    semGroupAutomations.set(`${groupId}:${automationId}`, {
      groupId, automationId, order: p.getUint16(16, true), title: textAt(p, 20, 32),
    })
  } else if (type === ENTITY.GROUP) {
    const id = p.getBigUint64(0, true)
    groups.set(`grp:${id}`, { key: { id }, record: decodeGroup(p, 8) })
  } else if (type === ENTITY.LOCATION) {
    const id = p.getBigUint64(0, true)
    locations.set(`loc:${id}`, { key: { id }, record: decodeLocation(p, 8) })
  } else if (type === ENTITY.WEATHER) {
    const id = p.getBigUint64(0, true)
    weatherMap.set(`weather:${id}`, { key: { id }, record: decodeWeather(p, 8) })
  } else if (type === ENTITY.WIFI_STATUS) {
    const id = p.getBigUint64(0, true)
    wifiStatusMap.set(`wifi:${id}`, { key: { id }, record: decodeWifiStatus(p, 8) })
  } else if (type === ENTITY.SETTINGS) {
    const id = p.getUint8(0)
    settingsMap.set(`set:${id}`, { key: { id }, record: decodeSettings(p, 1) })
  }
}

export function applyEvent(p) {
  const ev = decodeValue(p, 16, 20)
  pushEvent({ kind: 'event', entity: p.getBigUint64(8, true), eventId: p.getInt32(0, true), valueKind: ev.kind, value: ev.value })
}

export function applyFrame(type, p) {
  if (type === MSG.EVENT) applyEvent(p)
  else if (SIZES[type]) applyEntity(type, p)
}

export function entityName(entity) {
  return semEntities.get(entity.toString())?.name ?? ''
}
export function deviceName(device) {
  return semDevices.get(device.toString())?.name ?? ''
}
export function entitiesOfDevice(device) {
  const out = []
  for (const e of semEntities.values()) if (e.deviceId === device) out.push(e)
  return out
}
export function entityProperties(entity) {
  const prefix = `${entity}:`
  const out = []
  for (const [key, st] of propStates) if (key.startsWith(prefix)) out.push(st)
  out.sort((a, b) => a.property - b.property)
  return out
}
export function semState(entity, property) {
  return propStates.get(`${entity}:${property}`)
}
export function capabilities(entity) {
  return propCapabilities.get(entity.toString()) || []
}
export function entityEvents(entity) {
  return propEvents.get(entity.toString()) || []
}
