import { useStore } from '../useStore.js'
import { uidHex, clusterName, describeProfile, describeDeviceId, hex16 } from '../zcl.js'
import StateAttr from '../components/StateAttr.jsx'

// Единственное место обычного UI, где показывается raw Zigbee (cluster/attr/zcl_type/raw).
// Это transitional Web↔Domain ABI; обычные страницы его не читают.
export default function Diagnostics() {
  const s = useStore()
  const devices = [...s.devices.values()]

  return (
    <section>
      <h2>Диагностика (raw Zigbee)</h2>
      {devices.length === 0 && <p className="muted pad">устройств нет</p>}
      {devices.map((d) => {
        const endpoints = [...s.endpoints.values()]
          .filter((e) => e.key.uid === d.key.uid)
          .sort((a, b) => a.key.ep - b.key.ep)
        const states = [...s.states.values()].filter((st) => st.key.uid === d.key.uid)
        return (
          <div className="card" key={uidHex(d.key.uid)}>
            <div className="card-head">
              <b>{d.record.name || d.record.model || uidHex(d.key.uid)}</b>
              <span className="uid">{uidHex(d.key.uid)}</span>
            </div>
            {endpoints.map((e) => (
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
              </div>
            ))}
            <h3>Атрибуты (сырые)</h3>
            <div className="attrs">
              {states.length === 0 && <span className="muted">атрибутов пока нет</span>}
              {states.map((st) => (
                <StateAttr key={`${st.key.cluster}:${st.key.attr}:${st.key.ep}`} uid={d.key.uid} st={st} />
              ))}
            </div>
          </div>
        )
      })}
    </section>
  )
}
