import { MSG, MSG3, decodeFrame, snapshot, ack } from './proto.js'
import { VALUE_KIND } from './semantics.js'
import { ENTITY_SYSTEM } from './system.js'

// Единый стор: WS-соединение + канонические сущности Domain (entity-native). React читает
// через useSyncExternalStore(store.subscribe, store.getVersion). Никаких числовых Domain-
// типов/сырых записей: каждая запись — entity-native сообщение v3.

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

// Декод entity-native rule-wire (зеркало web.c:web_rule_encode). Без ZCL.
function decodeRule(dv, base) {
  let o = base
  const enabled = dv.getUint8(o); o += 1
  const triggerKind = dv.getUint8(o); o += 1
  const trigger = { kind: triggerKind }
  if (triggerKind === 0) {
    trigger.entity = dv.getBigUint64(o, true); o += 8
    trigger.eventId = dv.getUint8(o); o += 1
  } else if (triggerKind === 1) {
    trigger.minutesOfDay = dv.getUint16(o, true); o += 2
    trigger.weekdayMask = dv.getUint8(o); o += 1
  } else if (triggerKind === 2) {
    trigger.entity = dv.getBigUint64(o, true); o += 8
    trigger.property = dv.getUint16(o, true); o += 2
    trigger.op = dv.getUint8(o); o += 1
    trigger.edge = dv.getUint8(o); o += 1
    trigger.value = dv.getFloat32(o, true); o += 4
    trigger.value2 = dv.getFloat32(o, true); o += 4
  }
  const conditionsCount = dv.getUint8(o); o += 1
  const conditions = []
  for (let i = 0; i < conditionsCount; i++) {
    conditions.push({
      entity: dv.getBigUint64(o, true),
      property: dv.getUint16(o + 8, true),
      op: dv.getUint8(o + 10),
      value: dv.getFloat32(o + 11, true),
      value2: dv.getFloat32(o + 15, true),
    })
    o += 19
  }
  const action = {
    entity: dv.getBigUint64(o, true),
    property: dv.getUint16(o + 8, true),
    action: dv.getUint8(o + 10),
    valueKind: dv.getUint8(o + 11),
  }
  o += 12
  if (action.valueKind === 1) { // SCALAR: u8 kind + u32/f32
    const kind = dv.getUint8(o); o += 1
    action.scalarKind = kind
    action.value = kind === 4 ? dv.getFloat32(o, true) : dv.getUint32(o, true)
    o += 4
  } else if (action.valueKind === 2) { // XY
    action.x = dv.getFloat32(o, true)
    action.y = dv.getFloat32(o + 4, true)
    o += 8
  }
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
  if (frame.type === MSG.GROUP_ITEM || frame.type === MSG.GROUP_ITEM_REMOVE) {
    const groupId = payload.getBigUint64(0, true)
    const entity = payload.getBigUint64(8, true)
    const property = payload.getUint16(16, true)
    const key = `${groupId}:${entity}:${property}`
    if (frame.type === MSG.GROUP_ITEM_REMOVE) {
      semGroupItems.delete(key)
    } else {
      semGroupItems.set(key, {
        groupId, entity, property,
        order: payload.getUint16(18, true),
        title: textAt(payload, 20, 32),
      })
    }
    emit()
    return
  }
  if (frame.type === MSG3.EVENT) {
    const ev = decodeValue(payload, 9, 10)
    pushEvent({ kind: 'event', entity: payload.getBigUint64(0, true), eventId: payload.getUint8(8), valueKind: ev.kind, value: ev.value })
    emit()
    return
  }
  if (frame.type === MSG3.AUTOMATION || frame.type === MSG3.AUTOMATION_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = id.toString()
    if (frame.type === MSG3.AUTOMATION_REMOVE) {
      semAutomations.delete(key)
    } else {
      semAutomations.set(key, { id, rule: decodeRule(payload, 8) })
    }
    emit()
    return
  }
  if (frame.type === MSG3.ENTITY || frame.type === MSG3.ENTITY_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = id.toString()
    if (frame.type === MSG3.ENTITY_REMOVE) {
      semEntities.delete(key)
    } else {
      semEntities.set(key, { id, deviceId: payload.getBigUint64(8, true), name: textAt(payload, 16, 32) })
    }
    emit()
    return
  }
  if (frame.type === MSG3.DEVICE || frame.type === MSG3.DEVICE_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = id.toString()
    if (frame.type === MSG3.DEVICE_REMOVE) {
      semDevices.delete(key)
    } else {
      semDevices.set(key, { id, parentId: payload.getBigUint64(8, true), name: textAt(payload, 16, 32) })
    }
    emit()
    return
  }
  if (frame.type === MSG3.PROPERTY_STATE || frame.type === MSG3.PROPERTY_STATE_REMOVE) {
    const entity = payload.getBigUint64(0, true)
    const property = payload.getUint16(8, true)
    const key = `${entity}:${property}`
    const prev = propStates.get(key)
    if (frame.type === MSG3.PROPERTY_STATE_REMOVE) {
      propStates.delete(key)
      if (prev) pushEvent({ kind: 'state', entity, property, removed: true })
    } else {
      const { kind, value } = decodeValue(payload, 10, 11)
      propStates.set(key, { entity, property, kind, value })
      // Журнал: только реальные изменения, system исключён (минутные тики забили бы лог).
      if (entity !== ENTITY_SYSTEM && (!prev || prev.value !== value)) {
        pushEvent({ kind: 'state', entity, property, valueKind: kind, value })
      }
    }
    emit()
    return
  }
  if (frame.type === MSG3.CAPABILITIES || frame.type === MSG3.CAPABILITIES_REMOVE) {
    const key = payload.getBigUint64(0, true).toString()
    if (frame.type === MSG3.CAPABILITIES_REMOVE) {
      propCapabilities.delete(key)
      propEvents.delete(key)
    } else {
      const count = payload.getUint8(8)
      let o = 9
      const caps = []
      for (let i = 0; i < count; i++) {
        const property = payload.getUint16(o, true); o += 2
        const ac = payload.getUint8(o); o += 1
        const actions = []
        for (let j = 0; j < ac; j++) actions.push(payload.getUint8(o++))
        caps.push({ property, actions })
      }
      const ec = payload.getUint8(o); o += 1
      const evs = []
      for (let i = 0; i < ec; i++) evs.push(payload.getUint8(o++))
      propCapabilities.set(key, caps)
      propEvents.set(key, evs)
    }
    emit()
    return
  }
  if (frame.type === MSG3.LOCATION || frame.type === MSG3.LOCATION_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = `loc:${id}`
    if (frame.type === MSG3.LOCATION_REMOVE) locations.delete(key)
    else {
      const record = decodeLocation(payload, 8)
      locations.set(key, { key: { id }, record })
      pushEvent({ kind: 'location', id, name: record.name })
    }
    emit(); return
  }
  if (frame.type === MSG3.WEATHER || frame.type === MSG3.WEATHER_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = `weather:${id}`
    if (frame.type === MSG3.WEATHER_REMOVE) weatherMap.delete(key)
    else {
      weatherMap.set(key, { key: { id }, record: decodeWeather(payload, 8) })
      pushEvent({ kind: 'weather', id })
    }
    emit(); return
  }
  if (frame.type === MSG3.WIFI_STATUS || frame.type === MSG3.WIFI_STATUS_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = `wifi:${id}`
    if (frame.type === MSG3.WIFI_STATUS_REMOVE) wifiStatusMap.delete(key)
    else {
      wifiStatusMap.set(key, { key: { id }, record: decodeWifiStatus(payload, 8) })
      pushEvent({ kind: 'wifi', id })
    }
    emit(); return
  }
  if (frame.type === MSG3.SETTINGS || frame.type === MSG3.SETTINGS_REMOVE) {
    const id = payload.getUint8(0)
    const key = `set:${id}`
    if (frame.type === MSG3.SETTINGS_REMOVE) settingsMap.delete(key)
    else {
      settingsMap.set(key, { key: { id }, record: decodeSettings(payload, 1) })
      pushEvent({ kind: 'settings', id })
    }
    emit(); return
  }
  if (frame.type === MSG3.GROUP || frame.type === MSG3.GROUP_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = `grp:${id}`
    if (frame.type === MSG3.GROUP_REMOVE) groups.delete(key)
    else {
      const record = decodeGroup(payload, 8)
      groups.set(key, { key: { id }, record })
      pushEvent({ kind: 'group', id, name: record.title })
    }
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
