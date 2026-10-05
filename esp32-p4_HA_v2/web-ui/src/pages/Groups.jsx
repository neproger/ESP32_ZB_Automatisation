import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { groupPut, groupRemove, groupItemPut, groupItemRemove } from '../proto.js'
import { uidHex, clusterName, attrName, formatAttrValue } from '../zcl.js'

// Экраны Display (docs/clients/DISPLAY.md): group — экран, group_item — виджет.
// Виджет ссылается на состояние (device_uid, ep, cluster, attr).
function deviceName(devices, uid) {
  for (const { key, record } of devices.values()) {
    if (key.uid === uid) return record.name || record.model || uidHex(uid)
  }
  return uidHex(uid)
}

function statesOf(s) {
  return [...s.states.values()].sort(
    (a, b) => a.key.cluster - b.key.cluster || a.key.attr - b.key.attr || a.key.ep - b.key.ep,
  )
}

function stateKeyStr(k) {
  return `${k.uid}:${k.ep}:${k.cluster}:${k.attr}`
}
function parseStateKey(str) {
  const [uid, ep, cluster, attr] = str.split(':')
  return { uid: BigInt(uid), ep: Number(ep), cluster: Number(cluster), attr: Number(attr) }
}

function GroupCard({ group, items, devices, states }) {
  const [stateStr, setStateStr] = useState('')
  const [title, setTitle] = useState('')

  const addItem = () => {
    if (!stateStr) return
    const state = parseStateKey(stateStr)
    const order = items.length ? Math.max(...items.map((it) => it.record.order)) + 1 : 0
    store.send(groupItemPut(group.key.id, state, { order, title: title.trim() }))
    setStateStr('')
    setTitle('')
  }

  return (
    <div className="card">
      <div className="card-head">
        <span className="rule-id">{group.record.title || 'Экран #' + group.key.id}</span>
        <span className="spacer" />
        <button className="ghost danger" onClick={() => store.send(groupRemove(group.key.id))}>Удалить экран</button>
      </div>

      {items.length === 0 && <div className="muted">виджетов нет</div>}
      {items.map((it) => {
        const { state } = it.key
        const st = store.states.get(`st:${state.uid}:${state.ep}:${state.cluster}:${state.attr}`)
        const value = st ? formatAttrValue(state.cluster, state.attr, st.record.zclType, st.record.raw) : '—'
        return (
          <div className="flow" key={`gi:${stateKeyStr(state)}`}>
            <span className="node trigger">{it.record.title || attrName(state.cluster, state.attr)}</span>
            <span className="muted">
              {deviceName(devices, state.uid)} · {clusterName(state.cluster)} {attrName(state.cluster, state.attr)}
            </span>
            <span className="spacer" />
            <span className="node action">{value}</span>
            <button className="ghost danger" onClick={() => store.send(groupItemRemove(group.key.id, state))}>✕</button>
          </div>
        )
      })}

      <div className="af-line">
        <select value={stateStr} onChange={(e) => setStateStr(e.target.value)}>
          <option value="">(состояние)</option>
          {states.map((st) => (
            <option key={stateKeyStr(st.key)} value={stateKeyStr(st.key)}>
              {deviceName(devices, st.key.uid)} · {clusterName(st.key.cluster)} {attrName(st.key.cluster, st.key.attr)} (EP{st.key.ep})
            </option>
          ))}
        </select>
        <input type="text" placeholder="подпись (необязательно)" value={title}
          onChange={(e) => setTitle(e.target.value)} />
        <button onClick={addItem}>+ виджет</button>
      </div>
    </div>
  )
}

export default function Groups() {
  const s = useStore()
  const [newTitle, setNewTitle] = useState('')
  const groups = [...s.groups.values()].sort((a, b) => (a.key.id < b.key.id ? -1 : 1))
  const devices = [...s.devices.values()]
  const states = statesOf(s)

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
          devices={devices}
          states={states}
        />
      ))}
    </section>
  )
}
