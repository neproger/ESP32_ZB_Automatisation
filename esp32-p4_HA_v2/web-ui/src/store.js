import { MSG, decodeFrame, snapshot, snapshotState, parseSnapshot } from './proto.js'
import * as model from './model.js'
import { getEvents, setLive } from './events.js'

// Стор: WS (live deltas + STATE-снапшот) + HTTP bulk MODEL-файл (/model.bin). React читает
// через useSyncExternalStore(store.subscribe, store.getVersion). Хранение записи — model.js,
// журнал — events.js; здесь транспорт, порядок синхронизации и «версия».
//
// Порядок (не теряем изменения):
//   1) WS open → буферизуем кадры
//   2) GET /model.bin → применяем MODEL
//   3) WS SNAPSHOT_STATE → применяем STATE (актуальнее буфера)
//   4) проигрываем буфер → live
// MODEL-изменения живых клиентов приходят дельтами (re-GET не нужен); файл сервер
// пересобирает в фоне для новых подключений. Fallback: полный WS-snapshot.

let phase = 'live' // 'model' | 'state' | 'fallback' | 'live'
let pending = []
let version = 0
let status = 'connecting'
let socket = null
const listeners = new Set()
let lastSeq = -1
let reconnectDelay = 500

function setPhase(next) {
  phase = next
  setLive(next === 'live')
}
setPhase('live')

const emit = () => {
  version++
  for (const l of listeners) l()
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
  if (frame.seq === lastSeq) return
  lastSeq = frame.seq

  if (frame.type === MSG.SYNC_BEGIN) {
    if (phase === 'state') model.clearState()
    else model.clearAll()
    return
  }
  if (frame.type === MSG.SYNC_END) {
    for (const e of pending) model.applyFrame(e.type, new DataView(e.bytes.buffer))
    pending = []
    setPhase('live')
    emit()
    return
  }
  if (phase !== 'live') {
    pending.push({ type: frame.type, bytes: copyPayload(frame.payload) })
    return
  }
  model.applyFrame(frame.type, frame.payload)
  emit()
}

function startSync() {
  setPhase('model')
  pending = []
  fetch(`${location.origin}/model.bin`, { cache: 'no-store' })
    .then((r) => { if (!r.ok) throw new Error(`http ${r.status}`); return r.arrayBuffer() })
    .then((buf) => {
      const recs = parseSnapshot(buf)
      if (!recs) throw new Error('bad model')
      model.clearModel()
      for (const r of recs) model.applyEntity(r.type, r.view)
      setPhase('state')
      if (socket && socket.readyState === WebSocket.OPEN) socket.send(snapshotState().bytes)
    })
    .catch((err) => {
      console.warn('model.bin unavailable, full WS snapshot:', err)
      setPhase('fallback')
      pending = []
      if (socket && socket.readyState === WebSocket.OPEN) socket.send(snapshot().bytes)
    })
}

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
    setPhase('live')
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
    return model.locations
  },
  get groups() {
    return model.groups
  },
  get weather() {
    return model.weatherMap
  },
  get wifiStatus() {
    return model.wifiStatusMap
  },
  get settings() {
    return model.settingsMap
  },
  get events() {
    return getEvents()
  },
  get semAutomations() {
    return model.semAutomations
  },
  get semGroupItems() {
    return model.semGroupItems
  },
  get semGroupAutomations() {
    return model.semGroupAutomations
  },
  get semEntities() {
    return model.semEntities
  },
  get semDevices() {
    return model.semDevices
  },
  entityName: model.entityName,
  deviceName: model.deviceName,
  entitiesOfDevice: model.entitiesOfDevice,
  entityProperties: model.entityProperties,
  semState: model.semState,
  capabilities: model.capabilities,
  entityEvents: model.entityEvents,
  send(frame) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(frame.bytes)
  },
}
