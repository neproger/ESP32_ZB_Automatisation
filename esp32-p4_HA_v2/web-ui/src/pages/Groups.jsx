import { useEffect, useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { groupPut, groupRemove, groupItemPut, groupItemRemove } from '../proto.js'
import { uidHex, clusterName, attrName, formatAttrValue, widgetKind } from '../zcl.js'

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
  const effState = stateStr || (states[0] ? stateKeyStr(states[0].key) : '')

  const pickDevice = (u) => {
    setUid(u)
    const list = devs.find((g) => g.uid === u)?.list || []
    setStateStr(list[0] ? stateKeyStr(list[0].key) : '')
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
    <div className="af-section">
      <div className="af-section-head"><span>{item ? 'Виджет' : 'Новый виджет'}</span></div>

      <div className="af-line">Устройство:
        <select value={effUid} onChange={(e) => pickDevice(e.target.value)}>
          {devs.length === 0 && <option value="">(нет устройств с состояниями)</option>}
          {devs.map((g) => <option key={g.uid} value={g.uid}>{g.name}</option>)}
        </select>
      </div>

      <div className="af-line">Состояние:
        <select value={effState} onChange={(e) => setStateStr(e.target.value)}>
          <option value="">(состояние)</option>
          {states.map((st) => (
            <option key={stateKeyStr(st.key)} value={stateKeyStr(st.key)}>
              {clusterName(st.key.cluster)} {attrName(st.key.cluster, st.key.attr)} (EP{st.key.ep}) · {widgetKind(st.key.cluster, st.key.attr)}
            </option>
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

function GroupCard({ group, items, s }) {
  const [title, setTitle] = useState(group.record.title)
  useEffect(() => setTitle(group.record.title), [group.record.title])
  const [form, setForm] = useState(null) // null | { item } (item=null → новый)

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
        <input className="name" value={title} placeholder="Название экрана"
          onChange={(e) => setTitle(e.target.value)}
          onBlur={() => store.send(groupPut(group.key.id, title.trim()))}
          onKeyDown={(e) => e.key === 'Enter' && e.target.blur()} />
        <span className="spacer" />
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

      {form && (
        <WidgetForm s={s} group={group} items={items} item={form.item} onClose={() => setForm(null)} />
      )}
    </div>
  )
}

export default function Groups() {
  const s = useStore()
  const [newTitle, setNewTitle] = useState('')
  const groups = [...s.groups.values()].sort((a, b) => (a.key.id < b.key.id ? -1 : 1))

  const nextId = () => {
    let max = 0n
    for (const { key } of s.groups.values()) if (key.id > max) max = key.id
    return max + 1n
  }

  return (
    <section>
      <h2>Экраны</h2>
      <div className="af-line">
        <input type="text" placeholder="название нового экрана" value={newTitle}
          onChange={(e) => setNewTitle(e.target.value)} />
        <button onClick={() => { store.send(groupPut(nextId(), newTitle.trim())); setNewTitle('') }}>
          + экран
        </button>
      </div>

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
    </section>
  )
}
