import { MSG, decodeFrame, snapshot } from './proto.js'
import { ENTITY, SCHEMA, decodeKey, decodeRecord, entityId } from './schema.js'

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
    return
  }
  if (frame.type === MSG.SYNC_END) {
    syncing = false
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
  get automations() {
    return maps[ENTITY.AUTOMATION]
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
  isMarkedForRemoval(uid) {
    return maps[ENTITY.DEVICE_REMOVE].has(`rm:${uid}`)
  },
  send(frame) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(frame.bytes)
  },
}
