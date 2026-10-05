import { useStore } from '../useStore.js'
import { uidHex, clusterName, attrName, formatAttrValue } from '../zcl.js'

// Экраны Display (docs/clients/DISPLAY.md): group — экран, group_item — виджет.
function deviceName(devices, uid) {
  for (const { key, record } of devices.values()) {
    if (key.uid === uid) return record.name || record.model || uidHex(uid)
  }
  return uidHex(uid)
}

export default function Groups() {
  const s = useStore()
  const groups = [...s.groups.values()].sort((a, b) => (a.key.id < b.key.id ? -1 : 1))

  return (
    <section>
      <h2>Экраны</h2>
      {groups.length === 0 && <p className="muted pad">экранов пока нет</p>}
      {groups.map((g) => {
        const items = [...s.groupItems.values()]
          .filter((it) => it.key.groupId === g.key.id)
          .sort((a, b) => a.record.order - b.record.order)
        return (
          <div className="card" key={'grp:' + g.key.id}>
            <div className="card-head">
              <span className="rule-id">{g.record.title || 'Экран #' + g.key.id}</span>
            </div>
            {items.length === 0 && <div className="muted">виджетов нет</div>}
            {items.map((it) => {
              const { state } = it.key
              const st = s.states.get(`st:${state.uid}:${state.ep}:${state.cluster}:${state.attr}`)
              const value = st
                ? formatAttrValue(state.cluster, state.attr, st.record.zclType, st.record.raw)
                : '—'
              return (
                <div className="flow" key={`gi:${it.key.groupId}:${state.uid}:${state.ep}:${state.cluster}:${state.attr}`}>
                  <span className="node trigger">{it.record.title || attrName(state.cluster, state.attr)}</span>
                  <span className="muted">
                    {deviceName(s.devices, state.uid)} · {clusterName(state.cluster)} {attrName(state.cluster, state.attr)}
                  </span>
                  <span className="spacer" />
                  <span className="node action">{value}</span>
                </div>
              )
            })}
          </div>
        )
      })}
    </section>
  )
}
