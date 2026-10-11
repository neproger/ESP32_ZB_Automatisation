import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { deviceRename, deviceRemove, entityRename } from '../proto.js'
import EntityWidgets from './EntityWidgets.jsx'
import { propertyName, propertyUnit } from '../semantics.js'

// Карточка-устройство (Logical Device): имя сверху, внутри — его Entity (контролы/значения),
// затем дочерние устройства (дерево). Ordinary UI — на canonical device_id/entity_id.

function formatValue(st) {
  if (st.value == null) return '—'
  if (typeof st.value === 'boolean') return st.value ? 'да' : 'нет'
  return `${Number(st.value).toFixed(1)} ${propertyUnit(st.property)}`
}

function EntitySection({ ent }) {
  const [name, setName] = useState(ent.name || '')
  useEffect(() => setName(ent.name || ''), [ent.name])
  const props = store.entityProperties(ent.id)

  return (
    <div className="entity">
      <input
        className="name"
        value={name}
        placeholder="(без имени)"
        onChange={(e) => setName(e.target.value)}
        onBlur={() => store.send(entityRename(ent.id, name))}
        onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
      />
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

export default function DeviceCard({ device, devices, s }) {
  const [name, setName] = useState(device.name || '')
  useEffect(() => setName(device.name || ''), [device.name])

  const entities = store.entitiesOfDevice(device.id)
  const children = devices.filter((x) => x.parentId === device.id)

  return (
    <div className="card">
      <div className="card-head">
        <input
          className="name"
          value={name}
          placeholder="(без имени)"
          onChange={(e) => setName(e.target.value)}
          onBlur={() => store.send(deviceRename(device.id, name))}
          onKeyDown={(e) => e.key === 'Enter' && e.target.blur()}
        />
        <a className="uid" href={`#/device/${device.id.toString()}`}>{device.id.toString().slice(-8)}</a>
        <span className="spacer" />
        <button className="ghost danger" onClick={() => store.send(deviceRemove(device.id))}>Удалить</button>
      </div>

      {entities.map((e) => <EntitySection key={e.id.toString()} ent={e} />)}

      {entities.length === 0 && children.length === 0 && (
        <div className="attrs"><span className="muted">данных пока нет — устройство ещё не отчиталось</span></div>
      )}

      {children.map((c) => (
        <div className="child-device" key={c.id.toString()}>
          <DeviceCard device={c} devices={devices} s={s} />
        </div>
      ))}
    </div>
  )
}
