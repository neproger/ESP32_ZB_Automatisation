import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { groupPut, groupRemove, groupItemPut, groupItemRemove, groupAutomationPut, groupAutomationRemove, automationPut } from '../proto.js'
import Switch from '../components/Switch.jsx'
import Modal from '../components/Modal.jsx'
import EntityWidgets from '../components/EntityWidgets.jsx'
import WeatherWidget from '../components/WeatherWidget.jsx'

// Экраны Display: group — экран, group_item — плитка. Плитка ссылается на логическую
// Entity (endpoint), а её виджеты строятся по capability — как в HA v1 («виджеты от типа
// девайса»). Identity — entity_id, без uid/ep.

function entityName(s, entity) {
  return s.semEntities.get(entity.toString())?.name || entity.toString().slice(-6)
}

function automationName(a) {
  return a.rule?.name?.trim() || `Правило #${a.id.toString()}`
}

function removeTile(group, tile) {
  if (tile.automationId != null) store.send(groupAutomationRemove(group.key.id, tile.automationId))
  else store.send(groupItemRemove(group.key.id, tile.entity))
}

function putTileAt(group, tile, order) {
  if (tile.automationId != null) {
    store.send(groupAutomationPut(group.key.id, { automationId: tile.automationId, order, title: tile.title }))
  } else {
    store.send(groupItemPut(group.key.id, { entity: tile.entity, order, title: tile.title }))
  }
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

// Плитка экрана: Entity (виджеты по capability) или автоматизация (переключатель).
function TileForm({ s, group, nextOrder, item, onClose }) {
  const ents = entitiesWithProps(s)
  const autos = [...s.semAutomations.values()].sort((a, b) => (a.id < b.id ? -1 : 1))
  const [kind, setKind] = useState(item?.automationId != null ? 'automation' : 'entity')
  const [entity, setEntity] = useState(item?.entity != null ? item.entity.toString() : '')
  const [automationId, setAutomationId] = useState(item?.automationId != null ? item.automationId.toString() : '')
  const [title, setTitle] = useState(item?.title ?? '')

  const effEntity = entity || (ents[0]?.id.toString() ?? '')
  const effAuto = automationId || (autos[0]?.id.toString() ?? '')
  const canSave = kind === 'entity' ? !!effEntity : !!effAuto

  const putTile = (newOrder) => {
    if (kind === 'automation') {
      store.send(groupAutomationPut(group.key.id, { automationId: BigInt(effAuto), order: newOrder, title: title.trim() }))
    } else {
      store.send(groupItemPut(group.key.id, { entity: BigInt(effEntity), order: newOrder, title: title.trim() }))
    }
  }

  const save = () => {
    if (!canSave) return
    if (item) {
      const changedKind = (item.automationId != null) !== (kind === 'automation')
      const changedTarget = kind === 'entity'
        ? item.entity != null && item.entity.toString() !== effEntity
        : item.automationId != null && item.automationId.toString() !== effAuto
      if (changedKind) removeTile(group, item)
      else if (changedTarget) removeTile(group, item)
    }
    putTile(item ? item.order : nextOrder)
    onClose()
  }

  return (
    <div className="form-grid">
      <div className="af-line">Тип:
        <select value={kind} onChange={(e) => setKind(e.target.value)}>
          <option value="entity">Сущность</option>
          <option value="automation">Автоматизация</option>
        </select>
      </div>
      {kind === 'entity' ? (
        <div className="af-line">Сущность:
          <select value={effEntity} onChange={(e) => setEntity(e.target.value)}>
            {ents.length === 0 && <option value="">(нет сущностей со свойствами)</option>}
            {ents.map((g) => <option key={g.id.toString()} value={g.id.toString()}>{g.name}</option>)}
          </select>
        </div>
      ) : (
        <div className="af-line">Автоматизация:
          <select value={effAuto} onChange={(e) => setAutomationId(e.target.value)}>
            {autos.length === 0 && <option value="">(правил нет)</option>}
            {autos.map((a) => (
              <option key={a.id.toString()} value={a.id.toString()}>{automationName(a)}</option>
            ))}
          </select>
        </div>
      )}
      <div className="af-line">Подпись:
        <input type="text" value={title} placeholder="(необязательно)" onChange={(e) => setTitle(e.target.value)} />
      </div>
      <div className="af-line">
        <button onClick={save} disabled={!canSave}>Сохранить</button>
        {item && (
          <button className="ghost danger" onClick={() => { removeTile(group, item); onClose() }}>Удалить</button>
        )}
        <button className="ghost" onClick={onClose}>Отмена</button>
      </div>
    </div>
  )
}

function GroupCard({ group, tiles, s }) {
  const [form, setForm] = useState(null)
  const [edit, setEdit] = useState(false)
  const nextOrder = tiles.length ? Math.max(...tiles.map((t) => t.order)) + 1 : 0

  const move = (index, delta) => {
    const j = index + delta
    if (j < 0 || j >= tiles.length) return
    const a = tiles[index]
    const b = tiles[j]
    putTileAt(group, a, b.order)
    putTileAt(group, b, a.order)
  }

  return (
    <div className="card screen-tile">
      <div className="card-head">
        <span className="rule-id">{group.record.title || '(без названия)'}</span>
        <span className="spacer" />
        <button className="ghost" onClick={() => setEdit(true)}>Изменить</button>
        <button className="ghost danger" onClick={() => store.send(groupRemove(group.key.id))}>Удалить экран</button>
      </div>

      {tiles.length === 0 && <div className="muted">плиток нет</div>}
      <div className="screen-tiles">
        {tiles.map((it, i) => {
          const auto = it.automationId != null ? s.semAutomations.get(it.automationId.toString()) : null
          return (
            <div className="entity-tile" key={(auto ? 'a' : 'e') + (auto ? it.automationId : it.entity)}>
              <div className="entity-tile-head">
                <span className="entity-tile-title">
                  {it.title || (auto ? automationName(auto) : entityName(s, it.entity))}
                </span>
                <span className="spacer" />
                <button className="ghost" onClick={() => move(i, -1)} disabled={i === 0}>↑</button>
                <button className="ghost" onClick={() => move(i, 1)} disabled={i === tiles.length - 1}>↓</button>
                <button className="ghost" onClick={() => setForm({ item: it })}>Изменить</button>
              </div>
              {auto ? (
                <label className="af-line">Включено
                  <Switch checked={auto.rule.enabled}
                    onChange={(v) => store.send(automationPut(auto.id, { ...auto.rule, enabled: v }))} />
                </label>
              ) : (
                <EntityWidgets entity={it.entity} />
              )}
            </div>
          )
        })}
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
          <TileForm s={s} group={group} nextOrder={nextOrder} item={form.item} onClose={() => setForm(null)} />
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
            tiles={[
              ...[...s.semGroupItems.values()].filter((it) => it.groupId === g.key.id),
              ...[...s.semGroupAutomations.values()].filter((it) => it.groupId === g.key.id),
            ].sort((a, b) => a.order - b.order)}
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
