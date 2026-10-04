import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { renameDevice } from '../proto.js'
import { uidHex, clusterName } from '../schema.js'
import StateAttr from './StateAttr.jsx'

export default function DeviceCard({ dev }) {
  const { key, record } = dev
  const [name, setName] = useState(record.name)
  useEffect(() => setName(record.name), [record.name])

  const states = [...store.states.values()].filter((s) => s.key.uid === key.uid)
  const endpoints = [...store.endpoints.values()].filter((e) => e.key.uid === key.uid)

  const commit = () => store.send(renameDevice(key.uid, name))

  return (
    <div className="card">
      <div className="title">
        <input
          className="name"
          value={name}
          placeholder="(без имени)"
          onChange={(e) => setName(e.target.value)}
          onBlur={commit}
          onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
        />
        {record.model && <span className="tag">{record.model}</span>}
        <a className="uid" href={`#/device/${key.uid}`}>{uidHex(key.uid)}</a>
      </div>

      <div className="attrs">
        {states.length === 0 && <span className="muted">атрибутов пока нет</span>}
        {states.map((st) => (
          <StateAttr key={`${st.key.cluster}:${st.key.attr}:${st.key.ep}`} uid={key.uid} st={st} />
        ))}
      </div>

      {endpoints.length > 0 && (
        <div className="attrs">
          {endpoints.map((e) => (
            <span className="attr small" key={e.key.ep}>
              EP{e.key.ep}: {e.record.clusters.map((c) => clusterName(c.id)).join(', ') || '—'}
            </span>
          ))}
        </div>
      )}
    </div>
  )
}
