import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { renameDevice, removeDevice, cancelRemoveDevice } from '../proto.js'
import { uidHex } from '../zcl.js'
import EndpointWidgets from './EndpointWidgets.jsx'
import { propertyName, propertyUnit } from '../semantics.js'

function semStatesOf(uid) {
  const out = []
  for (const st of store.semStates.values()) if (st.uid === uid) out.push(st)
  out.sort((a, b) => a.property - b.property || a.ep - b.ep)
  return out
}
function formatSem(st) {
  if (st.value == null) return '—'
  if (typeof st.value === 'boolean') return st.value ? 'да' : 'нет'
  return `${Number(st.value).toFixed(1)} ${propertyUnit(st.property)}`
}

export default function DeviceCard({ dev }) {
  const { key, record } = dev
  const [name, setName] = useState(record.name)
  useEffect(() => setName(record.name), [record.name])

  const removing = store.isMarkedForRemoval(key.uid)
  const endpoints = [...store.endpoints.values()]
    .filter((e) => e.key.uid === key.uid)
    .sort((a, b) => a.key.ep - b.key.ep)
  const states = semStatesOf(key.uid)

  return (
    <div className="card">
      <div className="card-head">
        <input
          className="name"
          value={name}
          placeholder="(без имени)"
          onChange={(e) => setName(e.target.value)}
          onBlur={() => store.send(renameDevice(key.uid, name))}
          onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
        />
        {record.model && <span className="chip">{record.model}</span>}
        <a className="uid" href={`#/device/${key.uid}`}>{uidHex(key.uid)}</a>
        <span className="spacer" />
        {removing ? (
          <>
            <span className="chip warn">на удаление</span>
            <button className="ghost" onClick={() => store.send(cancelRemoveDevice(key.uid))}>Отменить</button>
          </>
        ) : (
          <button className="ghost danger" onClick={() => store.send(removeDevice(key.uid))}>Удалить</button>
        )}
      </div>

      {endpoints.map((e) => (
        <div className="endpoint" key={e.key.ep}>
          <div className="ep-head"><span className="ep-badge">EP{e.key.ep}</span></div>
          <EndpointWidgets uid={key.uid} ep={e.key.ep} />
        </div>
      ))}

      {states.length > 0 && (
        <div className="attrs">
          {states.map((st) => (
            <span className="attr small" key={`${st.ep}:${st.property}`}>
              {propertyName(st.property)}: <b>{formatSem(st)}</b>
            </span>
          ))}
        </div>
      )}

      {endpoints.length === 0 && states.length === 0 && (
        <div className="attrs"><span className="muted">данных пока нет — устройство не прислало отчёт</span></div>
      )}
    </div>
  )
}
