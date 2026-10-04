import { useEffect, useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { renameDevice } from '../proto.js'
import { uidHex, clusterName } from '../schema.js'
import StateAttr from '../components/StateAttr.jsx'

export default function DeviceDetail({ uid }) {
  const s = useStore()
  const target = BigInt(uid || '0')
  const dev = [...s.devices.values()].find((d) => d.key.uid === target)

  const [name, setName] = useState(dev?.record.name ?? '')
  useEffect(() => setName(dev?.record.name ?? ''), [dev?.record.name])

  if (!dev) {
    return (
      <section>
        <p className="pad"><a href="#/devices">← Устройства</a></p>
        <p className="muted pad">Устройство не найдено (0x{target.toString(16)})</p>
      </section>
    )
  }

  const states = [...s.states.values()].filter((st) => st.key.uid === target)
  const endpoints = [...s.endpoints.values()].filter((e) => e.key.uid === target)

  return (
    <section>
      <p className="pad"><a href="#/devices">← Устройства</a></p>
      <div className="card">
        <div className="title">
          <input
            className="name"
            value={name}
            placeholder="(без имени)"
            onChange={(e) => setName(e.target.value)}
            onBlur={() => store.send(renameDevice(target, name))}
            onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
          />
          {dev.record.model && <span className="tag">{dev.record.model}</span>}
          <span className="uid">{uidHex(target)}</span>
        </div>

        <h3>Атрибуты</h3>
        <div className="attrs">
          {states.length === 0 && <span className="muted">атрибутов пока нет</span>}
          {states.map((st) => (
            <StateAttr key={`${st.key.cluster}:${st.key.attr}:${st.key.ep}`} uid={target} st={st} />
          ))}
        </div>

        <h3>Endpoints</h3>
        {endpoints.length === 0 ? (
          <p className="muted">топология ещё не собрана</p>
        ) : (
          <table className="ep">
            <thead>
              <tr><th>EP</th><th>profile</th><th>device id</th><th>кластеры</th></tr>
            </thead>
            <tbody>
              {endpoints.map((e) => (
                <tr key={e.key.ep}>
                  <td>{e.key.ep}</td>
                  <td>0x{e.record.profile.toString(16)}</td>
                  <td>0x{e.record.deviceId.toString(16)}</td>
                  <td>
                    {e.record.clusters.map((c) => (
                      <span className="attr small" key={c.id}>
                        {clusterName(c.id)}{c.role === 1 ? ' (server)' : ' (client)'}
                      </span>
                    ))}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </section>
  )
}
