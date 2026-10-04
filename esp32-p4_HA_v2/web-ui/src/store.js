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
}

let version = 0
let status = 'connecting'
let socket = null
const listeners = new Set()

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
    for (const k in maps) maps[k].clear()
    return
  }
  if (frame.type === MSG.SYNC_END) {
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
  } else {
    const recDv = new DataView(payload.buffer, payload.byteOffset + 1 + schema.keySize, schema.recSize)
    maps[type].set(id, { key, record: decodeRecord(type, recDv) })
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
  isMarkedForRemoval(uid) {
    return maps[ENTITY.DEVICE_REMOVE].has(`rm:${uid}`)
  },
  send(frame) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(frame.bytes)
  },
}
