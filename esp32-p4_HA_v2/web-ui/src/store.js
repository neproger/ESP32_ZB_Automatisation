import { MSG, decodeFrame, snapshot } from './proto.js'
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

// Семантическое состояние: ключ (uid, ep, property) — идентичность, готовая к шагу A.
const semStates = new Map()
// Семантические правила: ключ — id. { id, representable, rule|null }.
const semAutomations = new Map()
// Семантические capabilities endpoint'а: ключ "uid:ep" → [{ property, actions }].
const semCapabilities = new Map()
// Semantic group items: ключ "group:uid:ep:property" → { groupId, uid, ep, property, order, title }.
const semGroupItems = new Map()

// Декод rule-wire (зеркало web.c:web_encode_sem_rule). Без ZCL.
function decodeSemRule(dv, base) {
  let o = base
  const enabled = dv.getUint8(o); o += 1
  const triggerKind = dv.getUint8(o); o += 1
  const trigger = { kind: triggerKind }
  if (triggerKind === 0) {
    trigger.deviceUid = dv.getBigUint64(o, true); o += 8
    trigger.eventId = dv.getUint8(o); o += 1
  } else if (triggerKind === 1) {
    trigger.minutesOfDay = dv.getUint16(o, true); o += 2
    trigger.weekdayMask = dv.getUint8(o); o += 1
  } else if (triggerKind === 2) {
    trigger.deviceUid = dv.getBigUint64(o, true); o += 8
    trigger.endpoint = dv.getUint8(o); o += 1
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
      deviceUid: dv.getBigUint64(o, true),
      endpoint: dv.getUint8(o + 8),
      property: dv.getUint16(o + 9, true),
      op: dv.getUint8(o + 11),
      value: dv.getFloat32(o + 12, true),
      value2: dv.getFloat32(o + 16, true),
    })
    o += 20
  }
  const action = {
    deviceUid: dv.getBigUint64(o, true),
    endpoint: dv.getUint8(o + 8),
    property: dv.getUint16(o + 9, true),
    action: dv.getUint8(o + 11),
    valueKind: dv.getUint8(o + 12),
  }
  o += 13
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
    semStates.clear()
    semAutomations.clear()
    semCapabilities.clear()
    semGroupItems.clear()
    return
  }
  if (frame.type === MSG.SYNC_END) {
    syncing = false
    emit()
    return
  }
  if (frame.type === MSG.SEMANTIC_CAPABILITIES || frame.type === MSG.SEMANTIC_CAPABILITIES_REMOVE) {
    const uid = payload.getBigUint64(0, true)
    const ep = payload.getUint8(8)
    const key = `${uid}:${ep}`
    if (frame.type === MSG.SEMANTIC_CAPABILITIES_REMOVE) {
      semCapabilities.delete(key)
    } else {
      const count = payload.getUint8(9)
      let o = 10
      const caps = []
      for (let i = 0; i < count; i++) {
        const property = payload.getUint16(o, true); o += 2
        const ac = payload.getUint8(o); o += 1
        const actions = []
        for (let j = 0; j < ac; j++) actions.push(payload.getUint8(o++))
        caps.push({ property, actions })
      }
      semCapabilities.set(key, caps)
    }
    emit()
    return
  }
  if (frame.type === MSG.SEMANTIC_GROUP_ITEM || frame.type === MSG.SEMANTIC_GROUP_ITEM_REMOVE) {
    const groupId = payload.getBigUint64(0, true)
    const uid = payload.getBigUint64(8, true)
    const ep = payload.getUint8(16)
    const property = payload.getUint16(17, true)
    const key = `${groupId}:${uid}:${ep}:${property}`
    if (frame.type === MSG.SEMANTIC_GROUP_ITEM_REMOVE) {
      semGroupItems.delete(key)
    } else {
      const tb = new Uint8Array(payload.buffer, payload.byteOffset + 21, 32)
      const end = tb.indexOf(0)
      semGroupItems.set(key, {
        groupId, uid, ep, property,
        order: payload.getUint16(19, true),
        title: new TextDecoder().decode(end === -1 ? tb : tb.subarray(0, end)),
      })
    }
    emit()
    return
  }
  if (frame.type === MSG.EVENT) {
    events.push({
      seq: ++eventSeq,
      ts: Date.now(),
      kind: 'event',
      sourceUid: payload.getBigUint64(1, true),
      endpoint: payload.getUint8(9),
      eventId: payload.getUint8(10),
    })
    if (events.length > EVENT_LOG_MAX) events.splice(0, events.length - EVENT_LOG_MAX)
    emit()
    return
  }
  if (frame.type === MSG.SEMANTIC_AUTOMATION || frame.type === MSG.SEMANTIC_AUTOMATION_REMOVE) {
    const id = payload.getBigUint64(0, true)
    const key = id.toString()
    if (frame.type === MSG.SEMANTIC_AUTOMATION_REMOVE) {
      semAutomations.delete(key)
    } else {
      const representable = payload.getUint8(8) === 1
      semAutomations.set(key, { id, representable, rule: representable ? decodeSemRule(payload, 9) : null })
    }
    emit()
    return
  }
  if (frame.type === MSG.SEMANTIC_STATE || frame.type === MSG.SEMANTIC_STATE_REMOVE) {
    const uid = payload.getBigUint64(0, true)
    const ep = payload.getUint8(8)
    const property = payload.getUint16(9, true)
    const key = `${uid}:${ep}:${property}`
    if (frame.type === MSG.SEMANTIC_STATE_REMOVE) {
      semStates.delete(key)
    } else {
      const kind = payload.getUint8(11)
      let value = null
      switch (kind) {
        case VALUE_KIND.BOOL: value = payload.getUint32(12, true) !== 0; break
        case VALUE_KIND.I32: value = payload.getInt32(12, true); break
        case VALUE_KIND.U32:
        case VALUE_KIND.ENUM: value = payload.getUint32(12, true); break
        case VALUE_KIND.FLOAT: value = payload.getFloat32(12, true); break
        default: value = null
      }
      semStates.set(key, { uid, ep, property, kind, value })
    }
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
  get semStates() {
    return semStates
  },
  get semAutomations() {
    return semAutomations
  },
  get semCapabilities() {
    return semCapabilities
  },
  get semGroupItems() {
    return semGroupItems
  },
  endpointCapabilities(uid, ep) {
    return semCapabilities.get(`${uid}:${ep}`) || []
  },
  // Семантическое состояние по (uid, ep, property): { uid, ep, property, kind, value } | undefined.
  semState(uid, ep, property) {
    return semStates.get(`${uid}:${ep}:${property}`)
  },
  isMarkedForRemoval(uid) {
    return maps[ENTITY.DEVICE_REMOVE].has(`rm:${uid}`)
  },
  send(frame) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(frame.bytes)
  },
}
