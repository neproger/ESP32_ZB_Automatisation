import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { renameDevice, removeDevice, cancelRemoveDevice } from '../proto.js'
import { uidHex, clusterName } from '../zcl.js'
import EndpointWidgets from './EndpointWidgets.jsx'
import StateAttr from './StateAttr.jsx'

export default function DeviceCard({ dev }) {
  const { key, record } = dev
  const [name, setName] = useState(record.name)
  useEffect(() => setName(record.name), [record.name])
  const removing = store.isMarkedForRemoval(key.uid)

  const endpoints = [...store.endpoints.values()]
    .filter((e) => e.key.uid === key.uid)
    .sort((a, b) => a.key.ep - b.key.ep)
  const states = [...store.states.values()].filter((s) => s.key.uid === key.uid)

  return (
    <div className="card">
      <div className="title">
        <input
          className="name"
          value={name}
          placeholder="(без имени)"
          onChange={(e) => setName(e.target.value)}
          onBlur={() => store.send(renameDevice(key.uid, name))}
          onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
        />
        {record.model && <span className="tag">{record.model}</span>}
        <a className="uid" href={`#/device/${key.uid}`}>{uidHex(key.uid)}</a>
        {removing ? (
          <>
            <span className="tag warn">на удаление</span>
            <button onClick={() => store.send(cancelRemoveDevice(key.uid))}>Отменить</button>
          </>
        ) : (
          <button onClick={() => store.send(removeDevice(key.uid))}>Удалить</button>
        )}
      </div>

      {endpoints.length > 0 &&
        endpoints.map((e) => (
          <div className="endpoint" key={e.key.ep}>
            <div className="ep-head">
              EP{e.key.ep}
              <span className="uid">
                {e.record.clusters.map((c) => clusterName(c.id) + (c.role === 1 ? '' : ' (client)')).join(', ') || '—'}
              </span>
            </div>
            <EndpointWidgets
              uid={key.uid}
              ep={e.key.ep}
              record={e.record}
              states={states.filter((s) => s.key.ep === e.key.ep)}
            />
          </div>
        ))}

      {/* Пока топология (endpoint) не собрана — показываем известные атрибуты. */}
      {endpoints.length === 0 && states.length > 0 && (
        <div className="attrs">
          {states.map((st) => (
            <StateAttr key={`${st.key.ep}:${st.key.cluster}:${st.key.attr}`} uid={key.uid} st={st} />
          ))}
        </div>
      )}

      {endpoints.length === 0 && states.length === 0 && (
        <div className="attrs"><span className="muted">данных пока нет — устройство не прислало отчёт</span></div>
      )}
    </div>
  )
}
