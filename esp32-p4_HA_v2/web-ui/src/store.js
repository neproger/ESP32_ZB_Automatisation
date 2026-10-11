import { MSG, ENTITY, SIZES, parseSnapshot, decodeFrame, snapshot, snapshotState, ack } from './proto.js'
import { VALUE_KIND } from './semantics.js'
import { ENTITY_SYSTEM } from './system.js'

// Стор: WS (live deltas + STATE-снапшот) + HTTP bulk MODEL-файл (/model.bin). React читает
// через useSyncExternalStore(store.subscribe, store.getVersion).
//
// Порядок (не теряем изменения):
//   1) WS open → буферизуем кадры
//   2) GET /model.bin → применяем MODEL
//   3) WS SNAPSHOT_STATE → применяем STATE (актуальнее буфера)
//   4) проигрываем буфер → live
// MODEL-изменения живых клиентов приходят дельтами (ре-GET не нужен); файл сервер
// пересобирает в фоне для новых подключений. Fallback: полный WS-snapshot.

const locations = new Map()
const weatherMap = new Map()
const wifiStatusMap = new Map()
const settingsMap = new Map()
const groups = new Map()
const semAutomations = new Map()
const semGroupItems = new Map()
const semEntities = new Map()
const semDevices = new Map()
const propStates = new Map()
const propCapabilities = new Map()
const propEvents = new Map()

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
  if (action.valueKind === 1) {
    action.scalarKind = dv.getInt32(base + 180, true)
    action.value = action.scalarKind === 4 ? dv.getFloat32(base + 184, true) : dv.getUint32(base + 184, true)
  } else if (action.valueKind === 2) {
    action.x = dv.getFloat32(base + 180, true)
    action.y = dv.getFloat32(base + 184, true)
  }
  return { enabled: dv.getUint8(base + 188) !== 0, trigger, conditions, action }
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

const EVENT_LOG_MAX = 200
const events = []
let eventSeq = 0
let phase = 'live' // 'model' | 'state' | 'fallback' | 'live'
let pending = []
let version = 0
let status = 'connecting'
let socket = null
let session = 0
const listeners = new Set()
let lastSeq = -1

function pushEvent(ev) {
  if (phase !== 'live') return
  events.push({ seq: ++eventSeq, ts: Date.now(), ...ev })
  if (events.length > EVENT_LOG_MAX) events.splice(0, events.length - EVENT_LOG_MAX)
}

const emit = () => {
  version++
  for (const l of listeners) l()
}

function clearModel() {
  for (const m of [semDevices, semEntities, propCapabilities, semAutomations, semGroupItems, groups, locations, settingsMap]) m.clear()
}
function clearState() {
  for (const m of [propStates, weatherMap, wifiStatusMap]) m.clear()
}
function clearAll() {
  clearModel()
  clearState()
}

// Применить один canonical record [key][record] по типу (без emit).
function applyEntity(type, p) {
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
    const property = p.getInt32(16, true)
    semGroupItems.set(`${groupId}:${entity}:${property}`, {
      groupId, entity, property, order: p.getUint16(24, true), title: textAt(p, 28, 32),
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

function applyEvent(p) {
  const ev = decodeValue(p, 16, 20)
  pushEvent({ kind: 'event', entity: p.getBigUint64(8, true), eventId: p.getInt32(0, true), valueKind: ev.kind, value: ev.value })
}

function applyFrame(type, p) {
  if (type === MSG.EVENT) applyEvent(p)
  else if (SIZES[type]) applyEntity(type, p)
}

function copyPayload(ds) {
  return new Uint8Array(ds.buffer, ds.byteOffset, ds.byteLength).slice()
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
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(ack(frame.seq).bytes)
  }
  if (frame.seq === lastSeq) return
  lastSeq = frame.seq

  if (frame.type === MSG.SYNC_BEGIN) {
    if (phase === 'state') clearState()
    else clearAll()
    return
  }
  if (frame.type === MSG.SYNC_END) {
    for (const e of pending) applyFrame(e.type, new DataView(e.bytes.buffer))
    pending = []
    phase = 'live'
    emit()
    return
  }
  if (phase !== 'live') {
    pending.push({ type: frame.type, bytes: copyPayload(frame.payload) })
    return
  }
  applyFrame(frame.type, frame.payload)
  emit()
}

function startSync() {
  phase = 'model'
  pending = []
  fetch(`${location.origin}/model.bin`, { cache: 'no-store' })
    .then((r) => { if (!r.ok) throw new Error(`http ${r.status}`); return r.arrayBuffer() })
    .then((buf) => {
      const recs = parseSnapshot(buf)
      if (!recs) throw new Error('bad model')
      clearModel()
      for (const r of recs) applyEntity(r.type, r.view)
      phase = 'state'
      if (socket && socket.readyState === WebSocket.OPEN) socket.send(snapshotState().bytes)
    })
    .catch((err) => {
      console.warn('model.bin unavailable, full WS snapshot:', err)
      phase = 'fallback'
      pending = []
      if (socket && socket.readyState === WebSocket.OPEN) socket.send(snapshot().bytes)
    })
}

let reconnectDelay = 500

function connect() {
  socket = new WebSocket(wsUrl())
  socket.binaryType = 'arraybuffer'
  socket.onopen = () => {
    status = 'online'
    lastSeq = -1
    reconnectDelay = 500
    emit()
    startSync()
  }
  socket.onclose = () => {
    status = 'offline'
    phase = 'live'
    emit()
    setTimeout(connect, reconnectDelay)
    reconnectDelay = Math.min(reconnectDelay * 2, 5000)
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
  entitiesOfDevice(device) {
    const out = []
    for (const e of semEntities.values()) if (e.deviceId === device) out.push(e)
    return out
  },
  entityProperties(entity) {
    const prefix = `${entity}:`
    const out = []
    for (const [key, st] of propStates) if (key.startsWith(prefix)) out.push(st)
    out.sort((a, b) => a.property - b.property)
    return out
  },
  semState(entity, property) {
    return propStates.get(`${entity}:${property}`)
  },
  capabilities(entity) {
    return propCapabilities.get(entity.toString()) || []
  },
  entityEvents(entity) {
    return propEvents.get(entity.toString()) || []
  },
  send(frame) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(frame.bytes)
  },
}
