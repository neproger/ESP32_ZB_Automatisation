import { MSG, decodeFrame, snapshot, snapshotState, parseSnapshot } from './proto.js'
import * as model from './model.js'
import { getEvents, setLive } from './events.js'

// Стор: WS (live deltas + STATE-снапшот) + HTTP bulk MODEL-файл (/model.bin). React читает
// через useSyncExternalStore(store.subscribe, store.getVersion). Хранение записи — model.js,
// журнал — events.js; здесь транспорт, порядок синхронизации и «версия».
//
// Порядок (не теряем изменения):
//   1) WS open → SNAPSHOT_STATE. Сервер атомарно регистрирует клиента и шлёт STATE,
//      поэтому дельты не теряются между регистрацией и снапшотом.
//   2) по SYNC_BEGIN (регистрация уже произошла) → GET /model.bin, применяем MODEL.
//   3) применяем буферизованные кадры: STATE-снапшот, затем live-дельты.
//   4) /model.bin недоступен → полный WS-snapshot (fallback).

let mode = 'live' // 'sync' | 'live'
let pending = []
let version = 0
let status = 'connecting'
let socket = null
const listeners = new Set()
let lastSeq = -1
let reconnectDelay = 500
let syncId = 0
let modelRequested = false
let modelReady = false
let snapshotDone = false
let fullSync = false
let postSnapshotAt = null
let discardUntilEnd = false

setLive(false)

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

function applyFrame(type, payload) {
  if (type === MSG.REMOVE) model.applyRemove(payload)
  else model.applyFrame(type, payload)
}

function finishSync() {
  if (mode === 'live') return
  const frames = pending
  pending = []
  for (let i = 0; i < frames.length; i++) {
    // Всё после SYNC_END — уже live-дельты, а не часть снапшота.
    if (i === postSnapshotAt) setLive(true)
    applyFrame(frames[i].type, new DataView(frames[i].bytes.buffer))
  }
  mode = 'live'
  setLive(true)
  emit()
}

function requestModel() {
  if (modelRequested) return
  modelRequested = true
  const id = syncId
  fetch(`${location.origin}/model.bin`, { cache: 'no-store' })
    .then((r) => { if (!r.ok) throw new Error(`http ${r.status}`); return r.arrayBuffer() })
    .then((buf) => {
      if (id !== syncId) return
      const recs = parseSnapshot(buf)
      if (!recs) throw new Error('bad model')
      model.clearModel()
      for (const r of recs) model.applyEntity(r.type, r.view)
      modelReady = true
      if (snapshotDone) finishSync()
    })
    .catch((err) => {
      if (id !== syncId) return
      console.warn('model.bin unavailable, full WS snapshot:', err)
      // Старый STATE-снапшот мог прийти не до конца: его хвост отбрасываем,
      // полный снапшот авторитетнее.
      discardUntilEnd = !snapshotDone
      fullSync = true
      modelReady = true
      snapshotDone = false
      postSnapshotAt = null
      pending = []
      if (socket && socket.readyState === WebSocket.OPEN) socket.send(snapshot().bytes)
    })
}

function beginSync() {
  syncId++
  mode = 'sync'
  pending = []
  modelRequested = false
  modelReady = false
  snapshotDone = false
  fullSync = false
  postSnapshotAt = null
  discardUntilEnd = false
  setLive(false)
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(snapshotState().bytes)
}

function onFrame(buf) {
  const frame = decodeFrame(buf)
  if (!frame) return
  if (frame.seq === lastSeq) return
  lastSeq = frame.seq

  if (mode === 'live') {
    applyFrame(frame.type, frame.payload)
    emit()
    return
  }

  if (discardUntilEnd) {
    if (frame.type === MSG.SYNC_END) {
      discardUntilEnd = false
      return
    }
    // Если хвост старого снапшота потерялся, новый SYNC_BEGIN не глотаем.
    if (frame.type !== MSG.SYNC_BEGIN) return
    discardUntilEnd = false
  }

  if (frame.type === MSG.SYNC_BEGIN) {
    // Снапшот и регистрация на сервере атомарны; всё, что пришло раньше,
    // старше снапшота и отбрасывается.
    pending = []
    postSnapshotAt = null
    if (fullSync) model.clearAll()
    else model.clearState()
    // Регистрация уже произошла — model.bin, собранный после неё, не отстанет.
    if (!fullSync) requestModel()
    return
  }
  if (frame.type === MSG.SYNC_END) {
    snapshotDone = true
    postSnapshotAt = pending.length
    if (modelReady) finishSync()
    return
  }
  pending.push({ type: frame.type, bytes: copyPayload(frame.payload) })
}

function connect() {
  socket = new WebSocket(wsUrl())
  socket.binaryType = 'arraybuffer'
  socket.onopen = () => {
    status = 'online'
    lastSeq = -1
    reconnectDelay = 500
    emit()
    beginSync()
  }
  socket.onclose = () => {
    status = 'offline'
    syncId++ // инвалидирует незавершённый /model.bin
    mode = 'live'
    pending = []
    setLive(false)
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
