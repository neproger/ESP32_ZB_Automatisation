import { useEffect, useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { entityRename } from '../proto.js'
import EntityWidgets from '../components/EntityWidgets.jsx'
import { propertyName, propertyUnit } from '../semantics.js'

// Детали логической Entity: имя + управление по capabilities + значения свойств.

function formatValue(st) {
  if (st.value == null) return '—'
  if (typeof st.value === 'boolean') return st.value ? 'да' : 'нет'
  return `${Number(st.value).toFixed(1)} ${propertyUnit(st.property)}`
}

export default function EntityDetail({ id }) {
  const s = useStore()
  const target = BigInt(id || '0')
  const ent = s.semEntities.get(target.toString())

  const [name, setName] = useState(ent?.name ?? '')
  useEffect(() => setName(ent?.name ?? ''), [ent?.name])

  if (!ent) {
    return (
      <section>
        <p className="pad"><a href="#/entities">← Сущности</a></p>
        <p className="muted pad">Сущность не найдена ({target.toString()})</p>
      </section>
    )
  }

  const props = store.entityProperties(target)

  return (
    <section>
      <p className="pad"><a href="#/entities">← Сущности</a> · <a href="#/diagnostics">Диагностика</a></p>
      <div className="card">
        <div className="title">
          <input
            className="name"
            value={name}
            placeholder="(без имени)"
            onChange={(e) => setName(e.target.value)}
            onBlur={() => store.send(entityRename(target, name))}
            onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
          />
          <span className="uid">{target.toString()}</span>
        </div>

        <h3>Управление</h3>
        <EntityWidgets entity={target} />

        {props.length > 0 && (
          <>
            <h3>Свойства</h3>
            <div className="attrs">
              {props.map((st) => (
                <span className="attr small" key={st.property}>
                  {propertyName(st.property)}: <b>{formatValue(st)}</b>
                </span>
              ))}
            </div>
          </>
        )}
      </div>
    </section>
  )
}
