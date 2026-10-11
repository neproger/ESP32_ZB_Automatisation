import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { permitJoin, entityRename } from '../proto.js'
import DeviceCard from '../components/DeviceCard.jsx'
import EntityWidgets from '../components/EntityWidgets.jsx'
import { propertyName, propertyUnit } from '../semantics.js'

const JOIN_SECONDS = 180

// Список устройств (Logical Device → Entity). Карточка-устройство, дерево по parent;
// Entity без владельца — отдельно. Ordinary UI на canonical device_id/entity_id.

function formatValue(st) {
  if (st.value == null) return '—'
  if (typeof st.value === 'boolean') return st.value ? 'да' : 'нет'
  return `${Number(st.value).toFixed(1)} ${propertyUnit(st.property)}`
}

function UnassignedEntity({ ent }) {
  const [name, setName] = useState(ent.name || '')
  const props = store.entityProperties(ent.id)
  return (
    <div className="entity">
      <input className="name" value={name} placeholder="(без имени)"
        onChange={(e) => setName(e.target.value)}
        onBlur={() => store.send(entityRename(ent.id, name))}
        onKeyDown={(e) => e.key === 'Enter' && e.target.blur()} />
      <EntityWidgets entity={ent.id} />
      {props.length > 0 && (
        <div className="attrs">
          {props.map((st) => (
            <span className="attr small" key={st.property}>
              {propertyName(st.property)}: <b>{formatValue(st)}</b>
            </span>
          ))}
        </div>
      )}
    </div>
  )
}

export default function Entities() {
  const s = useStore()
  const [status, setStatus] = useState('')

  const devices = [...s.semDevices.values()]
  const known = new Set(devices.map((d) => d.id.toString()))
  const roots = devices.filter((d) => d.parentId === 0n || !known.has(d.parentId.toString()))
  roots.sort((a, b) => (a.name || '').localeCompare(b.name || '') || (a.id < b.id ? -1 : 1))

  const unassigned = [...s.semEntities.values()].filter(
    (e) => e.deviceId === 0n || !known.has(e.deviceId.toString()),
  )

  const openNetwork = () => {
    store.send(permitJoin(JOIN_SECONDS))
    setStatus(`Сеть открыта на ${JOIN_SECONDS} с — включите устройство в режиме подключения`)
  }

  return (
    <section>
      <h2>
        Устройства
        <button onClick={openNetwork}>Подключить устройства</button>
      </h2>
      {status && <p className="muted pad">{status}</p>}
      {devices.length === 0 && unassigned.length === 0 && (
        <p className="muted pad">Устройств пока нет</p>
      )}
      {roots.map((d) => (
        <DeviceCard key={d.id.toString()} device={d} devices={devices} s={s} />
      ))}
      {unassigned.length > 0 && (
        <div className="card">
          <div className="card-head"><span className="muted">Без устройства</span></div>
          {unassigned.map((e) => <UnassignedEntity key={e.id.toString()} ent={e} />)}
        </div>
      )}
    </section>
  )
}
