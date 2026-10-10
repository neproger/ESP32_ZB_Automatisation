import { MSG, MSG3, DIAG, decodeFrame, snapshot } from './proto.js'
import { ENTITY, SCHEMA, decodeKey, decodeRecord, entityId } from './schema.js'
import { VALUE_KIND } from './semantics.js'

// Единый стор: WS-соединение + сырые сущности Domain. React читает через
// useSyncExternalStore(store.subscribe, store.getVersion).

const maps = {
  [ENTITY.DEVICE]: new Map(),
  [ENTITY.STATE]: new Map(),
  [ENTITY.ENDPOINT]: new Map(),
  [ENTITY.AUTOMATION]: new Map(),
  [ENTITY.DEVICE_REMOVE]: new Map(),
  [ENTITY.LOCATION]: new Map(),
  [ENTITY.GROUP]: new Map(),
  [ENTITY.GROUP_ITEM]: new Map(),
  [ENTITY.WEATHER]: new Map(),
  [ENTITY.WIFI_STATUS]: new Map(),
  [ENTITY.SETTINGS]: new Map(),
}

// Семантические правила: ключ — id. { id, rule }.
const semAutomations = new Map()
// Canonical group items: ключ "group:entity:property".
const semGroupItems = new Map()

// Canonical v3 (Шаг A, A6.1c): identity = entity_id. Это карты обычного UI.
const semEntities = new Map()      // entityStr → { id, name }
const propStates = new Map()       // `${entity}:${property}` → { entity, property, kind, value }
const propCapabilities = new Map() // entityStr → [{ property, actions }]
const dec = new TextDecoder()
const textAt = (payload, off, len) => {
  const tb = new Uint8Array(payload.buffer, payload.byteOffset + off, len)
  const end = tb.indexOf(0)
  return dec.decode(end === -1 ? tb : tb.subarray(0, end))
}

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

const EVENT_LOG_MAX = 200
const events = []
let eventSeq = 0
let syncing = false
let version = 0
let status = 'connecting'
let socket = null
const listeners = new Set()

function logEvent(action, entityType, key, record) {
  events.push({ seq: ++eventSeq, ts: Date.now(), action, entityType, key, record })
  if (events.length > EVENT_LOG_MAX) {
    events.splice(0, events.length - EVENT_LOG_MAX)
  }
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
  const { payload } = frame
  if (frame.type === MSG.SYNC_BEGIN) {
    syncing = true
    for (const k in maps) maps[k].clear()
    semAutomations.clear()
    semGroupItems.clear()
    semEntities.clear()
    propStates.clear()
    propCapabilities.clear()
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
    events.push({
      seq: ++eventSeq,
      ts: Date.now(),
      kind: 'event',
      sourceEntity: payload.getBigUint64(0, true),
      eventId: payload.getUint8(8),
    })
    if (events.length > EVENT_LOG_MAX) events.splice(0, events.length - EVENT_LOG_MAX)
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
      semEntities.set(key, { id, name: textAt(payload, 8, 32) })
    }
    emit()
    return
  }
  if (frame.type === MSG3.PROPERTY_STATE || frame.type === MSG3.PROPERTY_STATE_REMOVE) {
    const entity = payload.getBigUint64(0, true)
    const property = payload.getUint16(8, true)
    const key = `${entity}:${property}`
    if (frame.type === MSG3.PROPERTY_STATE_REMOVE) {
      propStates.delete(key)
    } else {
      const kind = payload.getUint8(10)
      let value = null
      switch (kind) {
        case VALUE_KIND.BOOL: value = payload.getUint32(11, true) !== 0; break
        case VALUE_KIND.I32: value = payload.getInt32(11, true); break
        case VALUE_KIND.U32:
        case VALUE_KIND.ENUM: value = payload.getUint32(11, true); break
        case VALUE_KIND.FLOAT: value = payload.getFloat32(11, true); break
        default: value = null
      }
      propStates.set(key, { entity, property, kind, value })
    }
    emit()
    return
  }
  if (frame.type === MSG3.CAPABILITIES || frame.type === MSG3.CAPABILITIES_REMOVE) {
    const key = payload.getBigUint64(0, true).toString()
    if (frame.type === MSG3.CAPABILITIES_REMOVE) {
      propCapabilities.delete(key)
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
      propCapabilities.set(key, caps)
    }
    emit()
    return
  }
  // Diagnostics raw Zigbee (A1.5): отдельный канал, наполняет raw-карты для Diagnostics.
  if (frame.type === DIAG.ZB_DEVICE || frame.type === DIAG.ZB_ENDPOINT || frame.type === DIAG.ZB_STATE) {
    const type = frame.type === DIAG.ZB_DEVICE ? ENTITY.DEVICE
      : frame.type === DIAG.ZB_ENDPOINT ? ENTITY.ENDPOINT : ENTITY.STATE
    const schema = SCHEMA[type]
    const keyDv = new DataView(payload.buffer, payload.byteOffset, schema.keySize)
    const key = decodeKey(type, keyDv)
    const id = entityId(type, key)
    const recDv = new DataView(payload.buffer, payload.byteOffset + schema.keySize, schema.recSize)
    const record = decodeRecord(type, recDv)
    maps[type].set(id, { key, record })
    if (!syncing) logEvent('upsert', type, key, record)
    emit()
    return
  }
  if (frame.type === DIAG.ZB_REMOVE) {
    const kind = payload.getUint8(0)
    const type = kind === 1 ? ENTITY.DEVICE : kind === 2 ? ENTITY.STATE : ENTITY.ENDPOINT
    const schema = SCHEMA[type]
    const keyDv = new DataView(payload.buffer, payload.byteOffset + 1, schema.keySize)
    const key = decodeKey(type, keyDv)
    maps[type].delete(entityId(type, key))
    if (!syncing) logEvent('remove', type, key, null)
    emit()
    return
  }
  if (frame.type !== MSG.ENTITY && frame.type !== MSG.ENTITY_REMOVE) return

  const type = payload.getUint8(0)
  const schema = SCHEMA[type]
  if (!schema) return
  const keyDv = new DataView(payload.buffer, payload.byteOffset + 1, schema.keySize)
  const key = decodeKey(type, keyDv)
  const id = entityId(type, key)
  if (frame.type === MSG.ENTITY_REMOVE) {
    maps[type].delete(id)
    if (!syncing) logEvent('remove', type, key, null)
  } else {
    const recDv = new DataView(payload.buffer, payload.byteOffset + 1 + schema.keySize, schema.recSize)
    const record = decodeRecord(type, recDv)
    maps[type].set(id, { key, record })
    if (!syncing) logEvent('upsert', type, key, record)
  }
  emit()
}

function connect() {
  socket = new WebSocket(wsUrl())
  socket.binaryType = 'arraybuffer'
  socket.onopen = () => {
    status = 'online'
    emit()
    socket.send(snapshot().bytes)
  }
  socket.onclose = () => {
    status = 'offline'
    emit()
    setTimeout(connect, 1500)
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
  get devices() {
    return maps[ENTITY.DEVICE]
  },
  get states() {
    return maps[ENTITY.STATE]
  },
  get endpoints() {
    return maps[ENTITY.ENDPOINT]
  },
  get removals() {
    return maps[ENTITY.DEVICE_REMOVE]
  },
  get locations() {
    return maps[ENTITY.LOCATION]
  },
  get groups() {
    return maps[ENTITY.GROUP]
  },
  get groupItems() {
    return maps[ENTITY.GROUP_ITEM]
  },
  get weather() {
    return maps[ENTITY.WEATHER]
  },
  get wifiStatus() {
    return maps[ENTITY.WIFI_STATUS]
  },
  get settings() {
    return maps[ENTITY.SETTINGS]
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

  // --- canonical v3: обычный UI работает только с entity_id ---------------------
  get semEntities() {
    return semEntities
  },
  entityName(entity) {
    return semEntities.get(entity.toString())?.name ?? ''
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
  isMarkedForRemoval(uid) {
    return maps[ENTITY.DEVICE_REMOVE].has(`rm:${uid}`)
  },
  send(frame) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(frame.bytes)
  },
}
