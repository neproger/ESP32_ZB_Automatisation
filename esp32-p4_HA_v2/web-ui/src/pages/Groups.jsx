import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { groupPut, groupRemove, semanticGroupItemPut, semanticGroupItemRemove } from '../proto.js'
import { propertyName, propertyUnit } from '../semantics.js'
import Modal from '../components/Modal.jsx'

// Экраны Display: group — экран, group_item — виджет. Виджет ссылается на canonical
// свойство Entity (entity_id, property); identity — entity, а не uid/ep.

function entityName(s, entity) {
  return s.semEntities.get(entity.toString())?.name || entity.toString().slice(-6)
}

// Свойства Entity: capabilities + известные значения.
function propsOf(entity) {
  const set = new Map()
  for (const cap of store.capabilities(entity)) set.set(cap.property, cap.property)
  for (const st of store.entityProperties(entity)) set.set(st.property, st.property)
  return [...set.values()].sort((a, b) => a - b)
}

// Entity, у которых есть свойства (для выпадающего списка).
function entitiesWithProps(s) {
  const out = []
  for (const e of s.semEntities.values()) {
    const props = propsOf(e.id)
    if (props.length > 0) out.push({ id: e.id, name: e.name || e.id.toString().slice(-6), props })
  }
  out.sort((a, b) => a.name.localeCompare(b.name))
  return out
}

function stateValue(s, entity, property) {
  const v = store.semState(entity, property)?.value
  if (v == null) return '—'
  if (typeof v === 'boolean') return v ? 'да' : 'нет'
  return `${Number(v).toFixed(1)} ${propertyUnit(property)}`
}

// Форма виджета: Entity → свойство → подпись.
function WidgetForm({ s, group, items, item, onClose }) {
  const ents = entitiesWithProps(s)
  const [entity, setEntity] = useState(item ? item.entity.toString() : '')
  const [property, setProperty] = useState(item ? item.property : 0)
  const [title, setTitle] = useState(item ? item.title : '')

  const effEntity = entity || (ents[0]?.id.toString() ?? '')
  const list = ents.find((g) => g.id.toString() === effEntity)?.props || []
  const hasProp = list.includes(property)
  const effProp = hasProp ? property : (list[0] ?? 0)

  const save = () => {
    if (!effEntity || !effProp) return
    const order = item ? item.order : (items.length ? Math.max(...items.map((it) => it.order)) + 1 : 0)
    if (item && (item.entity.toString() !== effEntity || item.property !== effProp)) {
      store.send(semanticGroupItemRemove(group.key.id, item.entity, item.property))
    }
    store.send(semanticGroupItemPut(group.key.id, { entity: BigInt(effEntity), property: effProp, order, title: title.trim() }))
    onClose()
  }

  return (
    <div className="form-grid">
      <div className="af-line">Сущность:
        <select value={effEntity} onChange={(e) => { setEntity(e.target.value); setProperty(0) }}>
          {ents.length === 0 && <option value="">(нет сущностей со свойствами)</option>}
          {ents.map((g) => <option key={g.id.toString()} value={g.id.toString()}>{g.name}</option>)}
        </select>
      </div>
      <div className="af-line">Свойство:
        <select value={effProp} onChange={(e) => setProperty(Number(e.target.value))}>
          <option value={0}>(свойство)</option>
          {list.map((p) => <option key={p} value={p}>{propertyName(p)}</option>)}
        </select>
      </div>
      <div className="af-line">Подпись:
        <input type="text" value={title} placeholder="(необязательно)" onChange={(e) => setTitle(e.target.value)} />
      </div>
      <div className="af-line">
        <button onClick={save} disabled={!effEntity || !effProp}>Сохранить</button>
        {item && (
          <button className="ghost danger" onClick={() => { store.send(semanticGroupItemRemove(group.key.id, item.entity, item.property)); onClose() }}>Удалить</button>
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
          <div className="flow" key={`${it.entity}:${it.property}`}>
            <span className="node action">{it.title || propertyName(it.property)}</span>
            <span className="muted">{entityName(s, it.entity)} · {propertyName(it.property)}</span>
            <span className="spacer" />
            <span className="sensor">{stateValue(s, it.entity, it.property)}</span>
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
