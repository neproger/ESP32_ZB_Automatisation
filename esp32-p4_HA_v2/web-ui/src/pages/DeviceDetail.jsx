import { useEffect, useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { renameDevice } from '../proto.js'
import { uidHex, clusterName, describeProfile, describeDeviceId, hex16 } from '../zcl.js'
import EndpointWidgets from '../components/EndpointWidgets.jsx'
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
  const endpoints = [...s.endpoints.values()]
    .filter((e) => e.key.uid === target)
    .sort((a, b) => a.key.ep - b.key.ep)

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

        <h3>Endpoints</h3>
        {endpoints.length === 0 ? (
          <p className="muted">топология ещё не собрана</p>
        ) : (
          endpoints.map((e) => (
            <div className="endpoint" key={e.key.ep}>
              <div className="ep-head">
                EP{e.key.ep}
                <span className="uid">
                  profile {describeProfile(e.record.profile) || hex16(e.record.profile)} · id{' '}
                  {describeDeviceId(e.record.deviceId) || hex16(e.record.deviceId)}
                </span>
              </div>
              <div className="attrs">
                {e.record.clusters.map((c) => (
                  <span className="attr small" key={c.id}>
                    {clusterName(c.id)}{c.role === 1 ? '' : ' (client)'}
                  </span>
                ))}
              </div>
              <EndpointWidgets uid={target} ep={e.key.ep} record={e.record} states={states.filter((st) => st.key.ep === e.key.ep)} />
            </div>
          ))
        )}

        <h3>Атрибуты (сырые)</h3>
        <div className="attrs">
          {states.length === 0 && <span className="muted">атрибутов пока нет</span>}
          {states.map((st) => (
            <StateAttr key={`${st.key.cluster}:${st.key.attr}:${st.key.ep}`} uid={target} st={st} />
          ))}
        </div>
      </div>
    </section>
  )
}
