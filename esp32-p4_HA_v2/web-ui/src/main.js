import './style.css'
import { MSG, decodeFrame, zbCommand, renameDevice, snapshot, automationPut, automationRemove } from './proto.js'
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
  const q = new URLSearchParams(location.search).get('ws')
  if (q) return q
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
  socket.onopen = () => {
    setLink(true)
    send(snapshot())
  }
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

const TRIGGER_CMDS = [['Toggle', 2], ['Вкл', 1], ['Выкл', 0]]
const ACTION_CLUSTERS = [['On/Off', 6], ['Level', 8]]
const ACTION_CMDS = { 6: [['Toggle', 2], ['Вкл', 1], ['Выкл', 0]], 8: [['Move to level', 0]] }

let editingId = null

function deviceName(uid) {
  if (uid === 0n) return '—'
  for (const [, { key, record }] of devices) {
    if (key.uid === uid) return record.name || record.model || uidHex(uid)
  }
  return uidHex(uid)
}

function el(tag, props = {}, text) {
  const e = document.createElement(tag)
  Object.assign(e, props)
  if (text != null) e.textContent = text
  return e
}

function fillDeviceSelect(sel, anyLabel) {
  sel.innerHTML = ''
  sel.append(el('option', { value: '0' }, anyLabel))
  for (const [, { key, record }] of devices) {
    sel.append(el('option', { value: key.uid.toString() },
      (record.name || record.model || uidHex(key.uid)) + ' (' + uidHex(key.uid) + ')'))
  }
}

function fillOptions(sel, pairs) {
  sel.innerHTML = ''
  for (const [label, value] of pairs) sel.append(el('option', { value: String(value) }, label))
}

function updateActionCmds() {
  const cluster = Number(document.getElementById('af-action-cluster').value)
  fillOptions(document.getElementById('af-action-cmd'), ACTION_CMDS[cluster] || ACTION_CMDS[6])
}

function openAutoForm(id) {
  editingId = id
  const form = document.getElementById('auto-form')
  form.hidden = false

  const g = (i) => document.getElementById(i)
  const enabled = el('input', { type: 'checkbox', id: 'af-enabled', checked: true })
  const trigDev = el('select', { id: 'af-trigger-dev' })
  const trigCmd = el('select', { id: 'af-trigger-cmd' })
  const actDev = el('select', { id: 'af-action-dev' })
  const actEp = el('input', { type: 'number', id: 'af-action-ep', min: 0, max: 255, value: 1 })
  const actCluster = el('select', { id: 'af-action-cluster' })
  const actCmd = el('select', { id: 'af-action-cmd' })

  fillDeviceSelect(trigDev, 'любое устройство')
  fillDeviceSelect(actDev, 'то же, что триггер')
  fillOptions(trigCmd, TRIGGER_CMDS)
  fillOptions(actCluster, ACTION_CLUSTERS)
  actCluster.addEventListener('change', updateActionCmds)
  updateActionCmds()

  const entry = id != null ? automations.get(`auto:${id}`) : null
  if (entry) {
    const r = entry.record
    enabled.checked = !!r.enabled
    trigDev.value = r.triggerUid.toString()
    trigCmd.value = String(r.triggerCmd)
    actDev.value = r.actionUid.toString()
    actEp.value = r.actionEp
    actCluster.value = String(r.actionCluster)
    updateActionCmds()
    actCmd.value = String(r.actionCmd)
  }

  const line = (labelText, field) => {
    const d = el('label', { className: 'af-line' })
    d.append(document.createTextNode(labelText + ' '), field)
    return d
  }
  const save = el('button', { type: 'submit' }, 'Сохранить')
  const cancel = el('button', { type: 'button' }, 'Отмена')
  cancel.addEventListener('click', closeAutoForm)
  const actions = el('div', { className: 'af-line' })
  actions.append(save, cancel)

  form.replaceChildren(
    line('Вкл', enabled),
    line('Триггер, устройство:', trigDev),
    line('команда:', trigCmd),
    line('Действие, устройство:', actDev),
    line('endpoint:', actEp),
    line('кластер:', actCluster),
    line('команда:', actCmd),
    actions,
  )
}

function closeAutoForm() {
  editingId = null
  document.getElementById('auto-form').hidden = true
}

function nextAutomationId() {
  let max = 0n
  for (const [, { key }] of automations) if (key.id > max) max = key.id
  return max + 1n
}

function submitAutoForm(event) {
  event.preventDefault()
  const g = (i) => document.getElementById(i)
  const id = editingId != null ? BigInt(editingId) : nextAutomationId()
  send(automationPut(id, {
    enabled: g('af-enabled').checked,
    triggerUid: BigInt(g('af-trigger-dev').value),
    triggerCmd: Number(g('af-trigger-cmd').value),
    actionUid: BigInt(g('af-action-dev').value),
    actionEp: Number(g('af-action-ep').value),
    actionCluster: Number(g('af-action-cluster').value),
    actionCmd: Number(g('af-action-cmd').value),
    actionArgs: [],
  }))
  closeAutoForm()
}

function renderAutomations() {
  const root = document.getElementById('automations')
  root.innerHTML = ''
  if (automations.size === 0) {
    root.innerHTML = '<p style="margin:0 1.25rem;color:#6b7280">нет правил</p>'
    return
  }
  for (const [, { key, record }] of automations) {
    const card = el('div', { className: 'device' })
    const title = el('div', { className: 'title' })

    const enabled = el('input', { type: 'checkbox', checked: !!record.enabled })
    enabled.addEventListener('change', () =>
      send(automationPut(key.id, { ...record, enabled: enabled.checked })))
    const label = el('span', {}, 'Правило #' + key.id)
    const edit = el('button', {}, 'Изменить')
    edit.addEventListener('click', () => openAutoForm(key.id))
    const del = el('button', {}, 'Удалить')
    del.addEventListener('click', () => send(automationRemove(key.id)))
    title.append(enabled, label, edit, del)

    const desc = el('div', { className: 'attrs' })
    desc.innerHTML =
      `<div class="attr">триггер: ${deviceName(record.triggerUid)} cmd=0x${record.triggerCmd.toString(16)}</div>` +
      `<div class="attr">→ ${deviceName(record.actionUid)} ep=${record.actionEp} ` +
      `cluster=0x${record.actionCluster.toString(16)} cmd=0x${record.actionCmd.toString(16)}</div>`

    card.append(title, desc)
    root.append(card)
  }
}

document.getElementById('auto-add').addEventListener('click', () => openAutoForm(null))
document.getElementById('auto-form').addEventListener('submit', submitAutoForm)

connect()
