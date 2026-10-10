import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { groupPut, groupRemove, semanticGroupItemPut, semanticGroupItemRemove } from '../proto.js'
import { uidHex } from '../zcl.js'
import { propertyName, propertyUnit } from '../semantics.js'
import Modal from '../components/Modal.jsx'

// Экраны Display: group — экран, semantic group item — виджет. Виджет ссылается на
// semantic-состояние (uid, ep, property); persistent запись остаётся raw (facade в BFF).

function deviceName(devices, uid) {
  for (const { key, record } of devices.values()) {
    if (key.uid === uid) return record.name || record.model || uidHex(uid)
  }
  return uidHex(uid)
}

// semantic-состояния, сгруппированные по устройству.
function devicesWithStates(s) {
  const byDev = new Map()
  for (const st of s.semStates.values()) {
    const uid = st.uid.toString()
    if (!byDev.has(uid)) byDev.set(uid, new Map())
    byDev.get(uid).set(`${st.ep}:${st.property}`, { ep: st.ep, property: st.property })
  }
  const out = []
  for (const [uid, m] of byDev) {
    out.push({ uid, name: deviceName(s.devices, BigInt(uid)), list: [...m.values()] })
  }
  out.sort((a, b) => a.name.localeCompare(b.name))
  return out
}

function stateValue(s, uid, ep, property) {
  const v = s.semState(uid, ep, property)?.value
  if (v == null) return '—'
  if (typeof v === 'boolean') return v ? 'да' : 'нет'
  return `${Number(v).toFixed(1)} ${propertyUnit(property)}`
}

// Форма виджета: устройство → состояние(property) → подпись. Общая для «новый»/«изменить».
function WidgetForm({ s, group, items, item, onClose }) {
  const devs = devicesWithStates(s)
  const [uid, setUid] = useState(item ? item.uid.toString() : '')
  const [stateKey, setStateKey] = useState(item ? `${item.ep}:${item.property}` : '')
  const [title, setTitle] = useState(item ? item.title : '')

  const effUid = uid || (devs[0]?.uid ?? '')
  const list = devs.find((g) => g.uid === effUid)?.list || []
  const hasKey = list.some((o) => `${o.ep}:${o.property}` === stateKey)
  const effKey = hasKey ? stateKey : list[0] ? `${list[0].ep}:${list[0].property}` : ''

  const save = () => {
    if (!effKey) return
    const [ep, property] = effKey.split(':').map(Number)
    const order = item ? item.order : (items.length ? Math.max(...items.map((it) => it.order)) + 1 : 0)
    if (item && (item.ep !== ep || item.property !== property)) {
      store.send(semanticGroupItemRemove(group.key.id, item.uid, item.ep, item.property))
    }
    store.send(semanticGroupItemPut(group.key.id, { uid: BigInt(effUid), ep, property, order, title: title.trim() }))
    onClose()
  }

  return (
    <div className="form-grid">
      <div className="af-line">Устройство:
        <select value={effUid} onChange={(e) => { setUid(e.target.value); setStateKey('') }}>
          {devs.length === 0 && <option value="">(нет устройств с состояниями)</option>}
          {devs.map((g) => <option key={g.uid} value={g.uid}>{g.name}</option>)}
        </select>
      </div>
      <div className="af-line">Состояние:
        <select value={effKey} onChange={(e) => setStateKey(e.target.value)}>
          <option value="">(состояние)</option>
          {list.map((o) => <option key={`${o.ep}:${o.property}`} value={`${o.ep}:${o.property}`}>{propertyName(o.property)} (EP{o.ep})</option>)}
        </select>
      </div>
      <div className="af-line">Подпись:
        <input type="text" value={title} placeholder="(необязательно)" onChange={(e) => setTitle(e.target.value)} />
      </div>
      <div className="af-line">
        <button onClick={save} disabled={!effKey}>Сохранить</button>
        {item && (
          <button className="ghost danger" onClick={() => { store.send(semanticGroupItemRemove(group.key.id, item.uid, item.ep, item.property)); onClose() }}>Удалить</button>
        )}
        <button className="ghost" onClick={onClose}>Отмена</button>
      </div>
    </div>
  )
}

function GroupCard({ group, items, s }) {
  const [form, setForm] = useState(null)
  const [edit, setEdit] = useState(false)

  const move = (index, delta) => {
    const j = index + delta
    if (j < 0 || j >= items.length) return
    const a = items[index]
    const b = items[j]
    store.send(semanticGroupItemPut(group.key.id, { ...a, order: b.order }))
    store.send(semanticGroupItemPut(group.key.id, { ...b, order: a.order }))
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
        {items.map((it, i) => (
          <div className="flow" key={`${it.uid}:${it.ep}:${it.property}`}>
            <span className="node action">{it.title || propertyName(it.property)}</span>
            <span className="muted">{deviceName(s.devices, it.uid)} · {propertyName(it.property)} (EP{it.ep})</span>
            <span className="spacer" />
            <span className="sensor">{stateValue(s, it.uid, it.ep, it.property)}</span>
            <button className="ghost" onClick={() => move(i, -1)} disabled={i === 0}>↑</button>
            <button className="ghost" onClick={() => move(i, 1)} disabled={i === items.length - 1}>↓</button>
            <button className="ghost" onClick={() => setForm({ item: it })}>Изменить</button>
          </div>
        ))}
      </div>

      <div className="af-line">
        <button onClick={() => setForm({ item: null })}>+ Добавить виджет</button>
      </div>

      {edit && (
        <Modal title="Экран" onClose={() => setEdit(false)}>
          <ScreenForm initialTitle={group.record.title}
            onSave={(title) => store.send(groupPut(group.key.id, title))}
            onClose={() => setEdit(false)} />
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

function ScreenForm({ initialTitle, onSave, onClose }) {
  const [title, setTitle] = useState(initialTitle || '')
  const save = () => { if (title.trim()) { onSave(title.trim()); onClose() } }
  return (
    <div className="form-grid">
      <div className="af-line">Название:
        <input type="text" autoFocus value={title} placeholder="например, Гостиная"
          onChange={(e) => setTitle(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && save()} />
      </div>
      <div className="af-line">
        <button className="primary" onClick={save} disabled={!title.trim()}>Сохранить</button>
        <button className="ghost" onClick={onClose}>Отмена</button>
      </div>
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
          items={[...s.semGroupItems.values()]
            .filter((it) => it.groupId === g.key.id)
            .sort((a, b) => a.order - b.order)}
          s={s}
        />
      ))}

      {newScreen && (
        <Modal title="Новый экран" onClose={() => setNewScreen(false)}>
          <ScreenForm onSave={(title) => store.send(groupPut(nextId(), title))} onClose={() => setNewScreen(false)} />
        </Modal>
      )}
    </section>
  )
}
