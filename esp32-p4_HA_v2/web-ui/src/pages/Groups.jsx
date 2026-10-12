import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { groupPut, groupRemove, groupItemPut, groupItemRemove } from '../proto.js'
import Modal from '../components/Modal.jsx'
import EntityWidgets from '../components/EntityWidgets.jsx'
import WeatherWidget from '../components/WeatherWidget.jsx'

// Экраны Display: group — экран, group_item — плитка. Плитка ссылается на логическую
// Entity (endpoint), а её виджеты строятся по capability — как в HA v1 («виджеты от типа
// девайса»). Identity — entity_id, без uid/ep.

function entityName(s, entity) {
  return s.semEntities.get(entity.toString())?.name || entity.toString().slice(-6)
}

// Entity, у которых есть capability или известные свойства (для выпадающего списка).
function entitiesWithProps(s) {
  const out = []
  for (const e of s.semEntities.values()) {
    const has = store.capabilities(e.id).length > 0 || store.entityProperties(e.id).length > 0
    if (!has) continue
    const dn = s.semDevices.get(e.deviceId?.toString())?.name || ''
    const name = e.name || (dn ? `${dn} · ${e.id.toString().slice(-4)}` : '#' + e.id.toString().slice(-6))
    out.push({ id: e.id, name })
  }
  out.sort((a, b) => a.name.localeCompare(b.name))
  return out
}

// Плитка: Entity → подпись.
function TileForm({ s, group, items, item, onClose }) {
  const ents = entitiesWithProps(s)
  const [entity, setEntity] = useState(item ? item.entity.toString() : '')
  const [title, setTitle] = useState(item ? item.title : '')

  const effEntity = entity || (ents[0]?.id.toString() ?? '')

  const save = () => {
    if (!effEntity) return
    const order = item ? item.order : (items.length ? Math.max(...items.map((it) => it.order)) + 1 : 0)
    if (item && item.entity.toString() !== effEntity) {
      store.send(groupItemRemove(group.key.id, item.entity))
    }
    store.send(groupItemPut(group.key.id, { entity: BigInt(effEntity), order, title: title.trim() }))
    onClose()
  }

  return (
    <div className="form-grid">
      <div className="af-line">Сущность:
        <select value={effEntity} onChange={(e) => setEntity(e.target.value)}>
          {ents.length === 0 && <option value="">(нет сущностей со свойствами)</option>}
          {ents.map((g) => <option key={g.id.toString()} value={g.id.toString()}>{g.name}</option>)}
        </select>
      </div>
      <div className="af-line">Подпись:
        <input type="text" value={title} placeholder="(необязательно)" onChange={(e) => setTitle(e.target.value)} />
      </div>
      <div className="af-line">
        <button onClick={save} disabled={!effEntity}>Сохранить</button>
        {item && (
          <button className="ghost danger" onClick={() => { store.send(groupItemRemove(group.key.id, item.entity)); onClose() }}>Удалить</button>
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
    store.send(groupItemPut(group.key.id, { ...a, order: b.order }))
    store.send(groupItemPut(group.key.id, { ...b, order: a.order }))
  }

  return (
    <div className="card screen-tile">
      <div className="card-head">
        <span className="rule-id">{group.record.title || '(без названия)'}</span>
        <span className="spacer" />
        <button className="ghost" onClick={() => setEdit(true)}>Изменить</button>
        <button className="ghost danger" onClick={() => store.send(groupRemove(group.key.id))}>Удалить экран</button>
      </div>

      {items.length === 0 && <div className="muted">плиток нет</div>}
      <div className="screen-tiles">
        {items.map((it, i) => (
          <div className="entity-tile" key={it.entity.toString()}>
            <div className="entity-tile-head">
              <span className="entity-tile-title">{it.title || entityName(s, it.entity)}</span>
              <span className="spacer" />
              <button className="ghost" onClick={() => move(i, -1)} disabled={i === 0}>↑</button>
              <button className="ghost" onClick={() => move(i, 1)} disabled={i === items.length - 1}>↓</button>
              <button className="ghost" onClick={() => setForm({ item: it })}>Изменить</button>
            </div>
            <EntityWidgets entity={it.entity} />
          </div>
        ))}
      </div>

      <div className="af-line">
        <button onClick={() => setForm({ item: null })}>+ Добавить плитку</button>
      </div>

      {edit && (
        <Modal title="Экран" onClose={() => setEdit(false)}>
          <ScreenForm initialTitle={group.record.title}
            onSave={(title) => store.send(groupPut(group.key.id, title))}
            onClose={() => setEdit(false)} />
        </Modal>
      )}
      {form && (
        <Modal title={form.item ? 'Изменить плитку' : 'Новая плитка'} onClose={() => setForm(null)}>
          <TileForm s={s} group={group} items={items} item={form.item} onClose={() => setForm(null)} />
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
        <span className="spacer" />
        <WeatherWidget />
        <button className="primary" onClick={() => setNewScreen(true)}>+ Экран</button>
      </h2>

      {groups.length === 0 && <p className="muted pad">экранов пока нет</p>}
      <div className="screens-grid">
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
      </div>

      {newScreen && (
        <Modal title="Новый экран" onClose={() => setNewScreen(false)}>
          <ScreenForm onSave={(title) => store.send(groupPut(nextId(), title))} onClose={() => setNewScreen(false)} />
        </Modal>
      )}
    </section>
  )
}
