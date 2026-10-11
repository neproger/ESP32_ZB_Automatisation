import { MSG, ENTITY, decodeFrame, snapshot, ack } from './proto.js'
import { VALUE_KIND } from './semantics.js'
import { ENTITY_SYSTEM } from './system.js'

// Единый стор: WS-соединение + канонические сущности Domain. React читает через
// useSyncExternalStore(store.subscribe, store.getVersion). Wire v3 — canonical structs
// verbatim (layout ha_model): type = ha_entity_t, payload = [key][record].

// Generic-сущности (identity — device_id или id; record как есть).
const locations = new Map()   // `loc:${id}`      → { key: { id }, record }
const weatherMap = new Map()  // `weather:${id}`  → { key: { id }, record }
const wifiStatusMap = new Map() // `wifi:${id}`   → { key: { id }, record }
const settingsMap = new Map() // `set:${id}`      → { key: { id }, record }
const groups = new Map()      // `grp:${id}`      → { key: { id }, record }

// Семантические правила: ключ — id. { id, rule }.
const semAutomations = new Map()
// Canonical group items: ключ "group:entity:property".
const semGroupItems = new Map()

// Canonical v3: identity = entity_id. Это карты обычного UI.
const semEntities = new Map()      // entityStr → { id, deviceId, name }
const semDevices = new Map()       // deviceStr → { id, parentId, name }
const propStates = new Map()       // `${entity}:${property}` → { entity, property, kind, value }
const propCapabilities = new Map() // entityStr → [{ property, actions }]
const propEvents = new Map()       // entityStr → [eventId] (press-события client-кластеров)

const dec = new TextDecoder()
function cstr(dv, off, len) {
  const bytes = new Uint8Array(dv.buffer, dv.byteOffset + off, len)
  const end = bytes.indexOf(0)
  return dec.decode(end === -1 ? bytes : bytes.subarray(0, end))
}
const textAt = (dv, off, len) => cstr(dv, off, len)

// --- record-декодеры (entity-native layout, без числового типа) --------------
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

// Декод canonical ha_rule_t (192) verbatim (layout ha_rule.h). Без ZCL.
const RULE_OFF_CONDITIONS_COUNT = 189
const RULE_OFF_ACTION = 190
const CONDITION_SIZE = 32

function decodeRule(dv, base) {
  const kind = dv.getUint8(base + 26)
  const trigger = {
    kind,
    entity: dv.getBigUint64(base + 16, true),
    eventId: dv.getInt32(base + 24, true),
    property: dv.getInt32(base + 28, true),
    value: dv.getFloat32(base + 32, true),
    value2: dv.getFloat32(base + 36, true),
    minutesOfDay: dv.getUint16(base + 40, true),
    op: dv.getUint8(base + 43),
    edge: dv.getUint8(base + 44),
    weekdayMask: dv.getUint8(base + 45),
  }
  const conditionsCount = dv.getUint8(base + RULE_OFF_CONDITIONS_COUNT)
  const conditions = []
  for (let i = 0; i < conditionsCount; i++) {
    const p = base + 48 + i * CONDITION_SIZE
    conditions.push({
      entity: dv.getBigUint64(p, true),
      property: dv.getInt32(p + 8, true),
      op: dv.getUint8(p + 16),
      value: dv.getFloat32(p + 20, true),
      value2: dv.getFloat32(p + 24, true),
    })
  }
  const action = {
    entity: dv.getBigUint64(base, true),
    property: dv.getInt32(base + 8, true),
    action: dv.getUint8(base + RULE_OFF_ACTION),
    valueKind: dv.getInt32(base + 176, true),
  }
  if (action.valueKind === 1) { // SCALAR: ha_value_t {kind i32, value}
    action.scalarKind = dv.getInt32(base + 180, true)
    action.value = action.scalarKind === 4
      ? dv.getFloat32(base + 184, true)
      : dv.getUint32(base + 184, true)
  } else if (action.valueKind === 2) { // XY
    action.x = dv.getFloat32(base + 180, true)
    action.y = dv.getFloat32(base + 184, true)
  }
  const enabled = dv.getUint8(base + 188) !== 0
  return { enabled, trigger, conditions, action }
}

// Значение из canonical DTO (u8 valueKind @k, u32 bits @k+1).
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

const EVENT_LOG_MAX = 200
const events = []
let eventSeq = 0
let syncing = false
let version = 0
let status = 'connecting'
let socket = null
const listeners = new Set()
// Stop-and-wait транспорт: seq сервер-присвоенный; дубликат (retry) применяем один раз.
let lastSeq = -1

// Журнал: одна typed-запись (kind + канонические поля). Snapshot (syncing) не логируется.
function pushEvent(ev) {
  if (syncing) return
  events.push({ seq: ++eventSeq, ts: Date.now(), ...ev })
  if (events.length > EVENT_LOG_MAX) events.splice(0, events.length - EVENT_LOG_MAX)
}

const emit = () => {
  version++
  for (const l of listeners) l()
}

function wsUrl() {
  const q = new URLSearchParams(location.search).get('ws')
  if (q) return q
  if (import.meta.env.VITE_WS_URL) return import.meta.env.VITE_WS_URL
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${location.host}/ws`
}

function onFrame(buf) {
  const frame = decodeFrame(buf)
  if (!frame) return
  // Подтверждаем каждый серверный кадр (at-least-once + dedup по seq = effective-once).
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(ack(frame.seq).bytes)
  }
  if (frame.seq === lastSeq) {
    return // дубликат после retry: ACK уже отправлен, payload не применяем
  }
  lastSeq = frame.seq
  const { payload } = frame
  if (frame.type === MSG.SYNC_BEGIN) {
    syncing = true
    for (const m of [locations, weatherMap, wifiStatusMap, settingsMap, groups, semAutomations,
      semGroupItems, semEntities, semDevices, propStates, propCapabilities, propEvents]) m.clear()
    return
  }
  if (frame.type === MSG.SYNC_END) {
    syncing = false
    emit()
    return
  }
  if (frame.type === MSG.EVENT) {
    const ev = decodeValue(payload, 16, 20)
    pushEvent({ kind: 'event', entity: payload.getBigUint64(8, true), eventId: payload.getInt32(0, true), valueKind: ev.kind, value: ev.value })
    emit()
    return
  }
  if (frame.type === ENTITY.AUTOMATION) {
    const id = payload.getBigUint64(0, true)
    semAutomations.set(id.toString(), { id, rule: decodeRule(payload, 8) })
    emit()
    return
  }
  if (frame.type === ENTITY.ENTITY) {
    const id = payload.getBigUint64(0, true)
    semEntities.set(id.toString(), { id, deviceId: payload.getBigUint64(8, true), name: textAt(payload, 16, 32) })
    emit()
    return
  }
  if (frame.type === ENTITY.LOGICAL_DEVICE) {
    const id = payload.getBigUint64(0, true)
    semDevices.set(id.toString(), { id, parentId: payload.getBigUint64(8, true), name: textAt(payload, 52, 32) })
    emit()
    return
  }
  if (frame.type === ENTITY.PROPERTY_STATE) {
    const entity = payload.getBigUint64(0, true)
    const property = payload.getInt32(8, true)
    const key = `${entity}:${property}`
    const prev = propStates.get(key)
    const { kind, value } = decodeValue(payload, 16, 20)
    propStates.set(key, { entity, property, kind, value })
    // Журнал: только реальные изменения, system исключён (минутные тики забили бы лог).
    if (entity !== ENTITY_SYSTEM && (!prev || prev.value !== value)) {
      pushEvent({ kind: 'state', entity, property, valueKind: kind, value })
    }
    emit()
    return
  }
  if (frame.type === ENTITY.CAPABILITIES) {
    const key = payload.getBigUint64(0, true).toString()
    const count = payload.getUint8(8)
    const caps = []
    for (let i = 0; i < count; i++) {
      const property = payload.getInt32(12 + i * 24, true)
      const ac = payload.getUint8(16 + i * 24)
      const actions = []
      for (let j = 0; j < ac; j++) actions.push(payload.getInt32(20 + i * 24 + j * 4, true))
      caps.push({ property, actions })
    }
    const ec = payload.getUint8(204)
    const evs = []
    for (let i = 0; i < ec; i++) evs.push(payload.getInt32(208 + i * 4, true))
    propCapabilities.set(key, caps)
    propEvents.set(key, evs)
    emit()
    return
  }
  if (frame.type === ENTITY.GROUP_ITEM) {
    const groupId = payload.getBigUint64(0, true)
    const entity = payload.getBigUint64(8, true)
    const property = payload.getInt32(16, true)
    semGroupItems.set(`${groupId}:${entity}:${property}`, {
      groupId, entity, property,
      order: payload.getUint16(24, true),
      title: textAt(payload, 28, 32),
    })
    emit()
    return
  }
  if (frame.type === ENTITY.LOCATION) {
    const id = payload.getBigUint64(0, true)
    const record = decodeLocation(payload, 8)
    locations.set(`loc:${id}`, { key: { id }, record })
    pushEvent({ kind: 'location', id, name: record.name })
    emit(); return
  }
  if (frame.type === ENTITY.WEATHER) {
    const id = payload.getBigUint64(0, true)
    weatherMap.set(`weather:${id}`, { key: { id }, record: decodeWeather(payload, 8) })
    pushEvent({ kind: 'weather', id })
    emit(); return
  }
  if (frame.type === ENTITY.WIFI_STATUS) {
    const id = payload.getBigUint64(0, true)
    wifiStatusMap.set(`wifi:${id}`, { key: { id }, record: decodeWifiStatus(payload, 8) })
    pushEvent({ kind: 'wifi', id })
    emit(); return
  }
  if (frame.type === ENTITY.SETTINGS) {
    const id = payload.getUint8(0)
    settingsMap.set(`set:${id}`, { key: { id }, record: decodeSettings(payload, 1) })
    pushEvent({ kind: 'settings', id })
    emit(); return
  }
  if (frame.type === ENTITY.GROUP) {
    const id = payload.getBigUint64(0, true)
    const record = decodeGroup(payload, 8)
    groups.set(`grp:${id}`, { key: { id }, record })
    pushEvent({ kind: 'group', id, name: record.title })
    emit(); return
  }
}

let reconnectDelay = 500

function connect() {
  socket = new WebSocket(wsUrl())
  socket.binaryType = 'arraybuffer'
  socket.onopen = () => {
    status = 'online'
    lastSeq = -1 // новая сессия: seq сервера снова с 0
    reconnectDelay = 500
    emit()
    socket.send(snapshot().bytes)
  }
  socket.onclose = () => {
    status = 'offline'
    emit()
    setTimeout(connect, reconnectDelay)
    reconnectDelay = Math.min(reconnectDelay * 2, 5000) // backoff против reconnect-шторма
  }
  socket.onerror = () => socket.close()
  socket.onmessage = (ev) => onFrame(ev.data)
}

connect()

export const store = {
  subscribe(cb) {
    listeners.add(cb)
    return () => listeners.delete(cb)
  },
  getVersion() {
    return version
  },
  get status() {
    return status
  },
  get locations() {
    return locations
  },
  get groups() {
    return groups
  },
  get weather() {
    return weatherMap
  },
  get wifiStatus() {
    return wifiStatusMap
  },
  get settings() {
    return settingsMap
  },
  get events() {
    return events
  },
  get semAutomations() {
    return semAutomations
  },
  get semGroupItems() {
    return semGroupItems
  },

  // --- canonical: обычный UI работает только с entity_id -----------------------
  get semEntities() {
    return semEntities
  },
  get semDevices() {
    return semDevices
  },
  entityName(entity) {
    return semEntities.get(entity.toString())?.name ?? ''
  },
  deviceName(device) {
    return semDevices.get(device.toString())?.name ?? ''
  },
  // Entity, принадлежащие Device (device_id == device).
  entitiesOfDevice(device) {
    const out = []
    for (const e of semEntities.values()) if (e.deviceId === device) out.push(e)
    return out
  },
  // Свойства Entity (с известным значением): [{ entity, property, kind, value }].
  entityProperties(entity) {
    const prefix = `${entity}:`
    const out = []
    for (const [key, st] of propStates) if (key.startsWith(prefix)) out.push(st)
    out.sort((a, b) => a.property - b.property)
    return out
  },
  // { entity, property, kind, value } | undefined.
  semState(entity, property) {
    return propStates.get(`${entity}:${property}`)
  },
  capabilities(entity) {
    return propCapabilities.get(entity.toString()) || []
  },
  // Press-события Entity (пусто — устройство их не шлёт).
  entityEvents(entity) {
    return propEvents.get(entity.toString()) || []
  },
  send(frame) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(frame.bytes)
  },
}
