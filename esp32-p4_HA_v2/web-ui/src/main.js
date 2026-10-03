import { MSG, decodeFrame, zbCommand, renameDevice } from './proto.js'
import {
  ENTITY,
  SCHEMA,
  decodeKey,
  decodeRecord,
  entityId,
  attrName,
  formatValue,
  CLUSTER_ONOFF,
  CMD_ONOFF,
} from './schema.js'

// ---- состояние (сырые сущности) ----
const devices = new Map()
const endpoints = new Map()
const states = new Map()
const automations = new Map()

let socket = null
let syncing = false

function wsUrl() {
  if (import.meta.env.VITE_WS_URL) return import.meta.env.VITE_WS_URL
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${location.host}/ws`
}

function setLink(ok) {
  const el = document.getElementById('link')
  el.textContent = ok ? 'подключено' : 'отключено'
  el.classList.toggle('ok', ok)
}

function connect() {
  socket = new WebSocket(wsUrl())
  socket.binaryType = 'arraybuffer'
  socket.onopen = () => setLink(true)
  socket.onclose = () => {
    setLink(false)
    setTimeout(connect, 1500)
  }
  socket.onerror = () => socket.close()
  socket.onmessage = (ev) => onFrame(ev.data)
}

function put(type, key, record) {
  const id = entityId(type, key)
  const entry = { key, record }
  switch (type) {
    case ENTITY.DEVICE:
      devices.set(id, entry)
      break
    case ENTITY.ENDPOINT:
      endpoints.set(id, entry)
      break
    case ENTITY.STATE:
      states.set(id, entry)
      break
    case ENTITY.AUTOMATION:
      automations.set(id, entry)
      break
  }
}

function remove(type, key) {
  const id = entityId(type, key)
  switch (type) {
    case ENTITY.DEVICE:
      devices.delete(id)
      break
    case ENTITY.ENDPOINT:
      endpoints.delete(id)
      break
    case ENTITY.STATE:
      states.delete(id)
      break
    case ENTITY.AUTOMATION:
      automations.delete(id)
      break
  }
}

function onFrame(buf) {
  const frame = decodeFrame(buf)
  if (!frame) return
  const { payload } = frame

  switch (frame.type) {
    case MSG.SYNC_BEGIN:
      syncing = true
      devices.clear()
      endpoints.clear()
      states.clear()
      automations.clear()
      break
    case MSG.SYNC_END:
      syncing = false
      render()
      break
    case MSG.ENTITY:
    case MSG.ENTITY_REMOVE: {
      const type = payload.getUint8(0)
      const schema = SCHEMA[type]
      if (!schema) return
      const keyDv = new DataView(payload.buffer, payload.byteOffset + 1, schema.keySize)
      const key = decodeKey(type, keyDv)
      if (frame.type === MSG.ENTITY_REMOVE) {
        remove(type, key)
      } else {
        const recDv = new DataView(payload.buffer, payload.byteOffset + 1 + schema.keySize, schema.recSize)
        put(type, key, decodeRecord(type, recDv))
      }
      if (!syncing) render()
      break
    }
  }
}

function send({ bytes }) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(bytes)
}

function uidHex(uid) {
  return '0x' + uid.toString(16).padStart(16, '0')
}

// ---- рендер ----
function render() {
  renderDevices()
  renderAutomations()
}

function renderDevices() {
  const root = document.getElementById('devices')
  root.innerHTML = ''
  if (devices.size === 0) {
    root.innerHTML = '<p style="margin:0 1.25rem;color:#6b7280">нет устройств</p>'
    return
  }

  for (const [id, { key, record }] of devices) {
    const card = document.createElement('div')
    card.className = 'device'

    const title = document.createElement('div')
    title.className = 'title'
    const name = document.createElement('input')
    name.type = 'text'
    name.value = record.name || ''
    name.placeholder = '(без имени)'
    name.addEventListener('change', () => send(renameDevice(key.uid, name.value)))
    const model = document.createElement('span')
    model.className = 'uid'
    model.textContent = record.model ? `модель: ${record.model}` : ''
    const uid = document.createElement('span')
    uid.className = 'uid'
    uid.textContent = uidHex(key.uid)
    title.append(name, model, uid)
    card.append(title)

    const attrs = document.createElement('div')
    attrs.className = 'attrs'
    for (const [, st] of states) {
      if (st.key.uid !== key.uid) continue
      attrs.append(renderState(key.uid, st))
    }
    card.append(attrs)
    root.append(card)
  }
}

function renderState(uid, st) {
  const { key, record } = st
  const el = document.createElement('div')
  el.className = 'attr'

  const label = document.createElement('b')
  label.textContent = attrName(key.cluster, key.attr) + ': '
  const value = document.createElement('span')
  value.textContent = formatValue(key.cluster, key.attr, record.zclType, record.raw)
  el.append(label, value)

  if (key.cluster === CLUSTER_ONOFF) {
    for (const [text, cmd] of [['Выкл', CMD_ONOFF.OFF], ['Вкл', CMD_ONOFF.ON], ['Toggle', CMD_ONOFF.TOGGLE]]) {
      const b = document.createElement('button')
      b.textContent = text
      b.addEventListener('click', () =>
        send(zbCommand({ uid, ep: key.ep, cluster: CLUSTER_ONOFF, command: cmd })),
      )
      el.append(b)
    }
  }
  return el
}

function renderAutomations() {
  const root = document.getElementById('automations')
  root.innerHTML = ''
  if (automations.size === 0) {
    root.innerHTML = '<p style="margin:0 1.25rem;color:#6b7280">нет правил</p>'
    return
  }
  for (const [, { key, record }] of automations) {
    const el = document.createElement('div')
    el.className = 'device'
    el.innerHTML =
      `<div class="title"><span>Правило #${key.id}</span>` +
      `<span class="uid">${record.enabled ? 'включено' : 'выключено'}</span></div>` +
      `<div class="attrs"><div class="attr">триггер cmd=0x${record.triggerCmd.toString(16)} ` +
      `→ cluster 0x${record.actionCluster.toString(16)} cmd=0x${record.actionCmd.toString(16)}</div></div>`
    root.append(el)
  }
}

connect()
