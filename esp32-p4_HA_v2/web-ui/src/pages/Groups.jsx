import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { groupPut, groupRemove, groupItemPut, groupItemRemove } from '../proto.js'
import { uidHex, clusterName, attrName, formatAttrValue, widgetKind } from '../zcl.js'
import Modal from '../components/Modal.jsx'

// Экраны Display (docs/clients/DISPLAY.md): group — экран, group_item — виджет.
// Виджет ссылается на состояние (device_uid, ep, cluster, attr); Display сам выбирает
// форму виджета по (cluster, attr) — показываем её как подсказку.
function deviceName(devices, uid) {
  for (const { key, record } of devices.values()) {
    if (key.uid === uid) return record.name || record.model || uidHex(uid)
  }
  return uidHex(uid)
}

function stateKeyStr(k) {
  return `${k.uid}:${k.ep}:${k.cluster}:${k.attr}`
}
function parseStateKey(str) {
  const [uid, ep, cluster, attr] = str.split(':')
  return { uid: BigInt(uid), ep: Number(ep), cluster: Number(cluster), attr: Number(attr) }
}

// Устройства, у которых есть состояния (только они годятся под виджет).
function devicesWithStates(s) {
  const byDev = new Map()
  for (const st of s.states.values()) {
    const uid = st.key.uid.toString()
    if (!byDev.has(uid)) byDev.set(uid, [])
    byDev.get(uid).push(st)
  }
  const out = []
  for (const [uid, list] of byDev) {
    list.sort((a, b) => a.key.cluster - b.key.cluster || a.key.attr - b.key.attr || a.key.ep - b.key.ep)
    out.push({ uid, name: deviceName(s.devices, BigInt(uid)), list })
  }
  out.sort((a, b) => a.name.localeCompare(b.name))
  return out
}

// Форма виджета: устройство → состояние → подпись. Используется и для нового, и для «Изменить».
function WidgetForm({ s, group, items, item, onClose }) {
  const devs = devicesWithStates(s)
  const [uid, setUid] = useState(item ? item.key.state.uid.toString() : '')
  const [stateStr, setStateStr] = useState(item ? stateKeyStr(item.key.state) : '')
  const [title, setTitle] = useState(item ? item.record.title : '')

  const effUid = uid || (devs[0]?.uid ?? '')
  const states = devs.find((g) => g.uid === effUid)?.list || []
  // Цвет (X/Y/hue/sat) — это один виджет: показываем одной строкой. Ключ берём от CurrentX
  // (Display для Color-виджета сам читает пару X/Y), иначе — любой Color-атрибут.
  const colorKey =
    states.find((st) => st.key.cluster === 0x0300 && st.key.attr === 0x0003) ||
    states.find((st) => st.key.cluster === 0x0300 && st.key.attr !== 0x0007)
  const listStates = []
  let colorAdded = false
  for (const st of states) {
    const isColor = st.key.cluster === 0x0300 && st.key.attr !== 0x0007
    if (isColor) {
      if (colorAdded) continue
      colorAdded = true
      listStates.push({ key: colorKey.key, label: 'Цвет' })
      continue
    }
    listStates.push({
      key: st.key,
      label: `${clusterName(st.key.cluster)} ${attrName(st.key.cluster, st.key.attr)} (EP${st.key.ep}) · ${widgetKind(st.key.cluster, st.key.attr)}`,
    })
  }
  const hasStateStr = listStates.some((o) => stateKeyStr(o.key) === stateStr)
  const effState = hasStateStr ? stateStr : listStates[0] ? stateKeyStr(listStates[0].key) : ''

  const pickDevice = (u) => {
    setUid(u)
    setStateStr('')
  }

  const save = () => {
    if (!effState) return
    const state = parseStateKey(effState)
    const order = item ? item.record.order : (items.length ? Math.max(...items.map((it) => it.record.order)) + 1 : 0)
    if (item && stateKeyStr(item.key.state) !== effState) {
      store.send(groupItemRemove(group.key.id, item.key.state))
    }
    store.send(groupItemPut(group.key.id, state, { order, title: title.trim() }))
    onClose()
  }

  return (
    <div className="form-grid">
      <div className="af-line">Устройство:
        <select value={effUid} onChange={(e) => pickDevice(e.target.value)}>
          {devs.length === 0 && <option value="">(нет устройств с состояниями)</option>}
          {devs.map((g) => <option key={g.uid} value={g.uid}>{g.name}</option>)}
        </select>
      </div>

      <div className="af-line">Состояние:
        <select value={effState} onChange={(e) => setStateStr(e.target.value)}>
          <option value="">(состояние)</option>
          {listStates.map((o) => (
            <option key={stateKeyStr(o.key)} value={stateKeyStr(o.key)}>{o.label}</option>
          ))}
        </select>
      </div>

      <div className="af-line">Подпись:
        <input type="text" value={title} placeholder="(необязательно)" onChange={(e) => setTitle(e.target.value)} />
      </div>

      <div className="af-line">
        <button onClick={save} disabled={!effState}>Сохранить</button>
        {item && (
          <button className="ghost danger" onClick={() => { store.send(groupItemRemove(group.key.id, item.key.state)); onClose() }}>
            Удалить
          </button>
        )}
        <button className="ghost" onClick={onClose}>Отмена</button>
      </div>
    </div>
  )
}

// Форма экрана (создание/переименование) — общая для «+ Экран» и «Изменить».
function ScreenForm({ initialTitle, onSave, onClose }) {
  const [title, setTitle] = useState(initialTitle || '')
  const save = () => {
    if (!title.trim()) return
    onSave(title.trim())
    onClose()
  }
  return (
    <div className="form-grid">
      <div className="af-line">Название:
        <input type="text" autoFocus value={title} placeholder="например, Гостиная"
          onChange={(e) => setTitle(e.target.value)}
          onKeyDown={(e) => e.key === 'Enter' && save()} />
      </div>
      <div className="af-line">
        <button className="primary" onClick={save} disabled={!title.trim()}>Сохранить</button>
        <button className="ghost" onClick={onClose}>Отмена</button>
      </div>
    </div>
  )
}

function GroupCard({ group, items, s }) {
  const [form, setForm] = useState(null) // null | { item } (item=null → новый виджет)
  const [edit, setEdit] = useState(false)

  const move = (index, delta) => {
    const j = index + delta
    if (j < 0 || j >= items.length) return
    const a = items[index]
    const b = items[j]
    store.send(groupItemPut(group.key.id, a.key.state, { order: b.record.order, title: a.record.title }))
    store.send(groupItemPut(group.key.id, b.key.state, { order: a.record.order, title: b.record.title }))
  }

  return (
    <div className="card">
      <div className="card-head">
        <span className="rule-id">{group.record.title || '(без названия)'}</span>
        <span className="spacer" />
        <button className="ghost" onClick={() => setEdit(true)}>Изменить</button>
        <button className="ghost danger" onClick={() => store.send(groupRemove(group.key.id))}>Удалить экран</button>
      </div>

      {items.length === 0 && <div className="muted">виджетов нет</div>}
      <div className="item-list">
        {items.map((it, i) => {
          const { state } = it.key
          const st = s.states.get(`st:${state.uid}:${state.ep}:${state.cluster}:${state.attr}`)
          const value = st ? formatAttrValue(state.cluster, state.attr, st.record.zclType, st.record.raw) : '—'
          return (
            <div className="flow" key={`gi:${stateKeyStr(state)}`}>
              <span className="node action">{it.record.title || widgetKind(state.cluster, state.attr)}</span>
              <span className="muted">
                {deviceName(s.devices, state.uid)} · {clusterName(state.cluster)} {attrName(state.cluster, state.attr)}
              </span>
              <span className="spacer" />
              <span className="sensor">{value}</span>
              <button className="ghost" onClick={() => move(i, -1)} disabled={i === 0}>↑</button>
              <button className="ghost" onClick={() => move(i, 1)} disabled={i === items.length - 1}>↓</button>
              <button className="ghost" onClick={() => setForm({ item: it })}>Изменить</button>
            </div>
          )
        })}
      </div>

      <div className="af-line">
        <button onClick={() => setForm({ item: null })}>+ Добавить виджет</button>
      </div>

      {edit && (
        <Modal title="Экран" onClose={() => setEdit(false)}>
          <ScreenForm
            initialTitle={group.record.title}
            onSave={(title) => store.send(groupPut(group.key.id, title))}
            onClose={() => setEdit(false)}
          />
        </Modal>
      )}

      {form && (
        <Modal title={form.item ? 'Изменить виджет' : 'Новый виджет'} onClose={() => setForm(null)}>
          <WidgetForm s={s} group={group} items={items} item={form.item} onClose={() => setForm(null)} />
        </Modal>
      )}
    </div>
  )
}

export default function Groups() {
  const s = useStore()
  const [newScreen, setNewScreen] = useState(false)
  const groups = [...s.groups.values()].sort((a, b) => (a.key.id < b.key.id ? -1 : 1))

  const nextId = () => {
    let max = 0n
    for (const { key } of s.groups.values()) if (key.id > max) max = key.id
    return max + 1n
  }

  return (
    <section>
      <h2>
        Экраны
        <button className="primary" onClick={() => setNewScreen(true)}>+ Экран</button>
      </h2>

      {groups.length === 0 && <p className="muted pad">экранов пока нет</p>}
      {groups.map((g) => (
        <GroupCard
          key={'grp:' + g.key.id}
          group={g}
          items={[...s.groupItems.values()]
            .filter((it) => it.key.groupId === g.key.id)
            .sort((a, b) => a.record.order - b.record.order)}
          s={s}
        />
      ))}

      {newScreen && (
        <Modal title="Новый экран" onClose={() => setNewScreen(false)}>
          <ScreenForm
            onSave={(title) => store.send(groupPut(nextId(), title))}
            onClose={() => setNewScreen(false)}
          />
        </Modal>
      )}
    </section>
  )
}
