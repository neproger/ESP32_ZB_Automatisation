import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { entityRename } from '../proto.js'
import EntityWidgets from './EntityWidgets.jsx'
import { propertyName, propertyUnit } from '../semantics.js'

// Карточка логической Entity: имя + контролы по capabilities + значения свойств.

function formatValue(st) {
  if (st.value == null) return '—'
  if (typeof st.value === 'boolean') return st.value ? 'да' : 'нет'
  return `${Number(st.value).toFixed(1)} ${propertyUnit(st.property)}`
}

export default function EntityCard({ ent }) {
  const { id, name } = ent
  const [value, setValue] = useState(name || '')
  useEffect(() => setValue(name || ''), [name])

  const props = store.entityProperties(id)
  const caps = store.capabilities(id)
  const key = id.toString()

  return (
    <div className="card">
      <div className="card-head">
        <input
          className="name"
          value={value}
          placeholder="(без имени)"
          onChange={(e) => setValue(e.target.value)}
          onBlur={() => store.send(entityRename(id, value))}
          onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
        />
        <a className="uid" href={`#/entity/${key}`}>{key}</a>
        <span className="spacer" />
      </div>

      <EntityWidgets entity={id} />

      {props.length > 0 && (
        <div className="attrs">
          {props.map((st) => (
            <span className="attr small" key={st.property}>
              {propertyName(st.property)}: <b>{formatValue(st)}</b>
            </span>
          ))}
        </div>
      )}

      {caps.length === 0 && props.length === 0 && (
        <div className="attrs"><span className="muted">данных пока нет — сущность ещё не отчиталась</span></div>
      )}
    </div>
  )
}
