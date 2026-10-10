import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { permitJoin } from '../proto.js'
import EntityCard from '../components/EntityCard.jsx'

const JOIN_SECONDS = 180

// Устройства сгруппированы как Logical Device → Entity (дерево по parent). Ordinary UI —
// на canonical entity_id/device_id; без uid/endpoint.

const deviceLabel = (d) => d.name || 'Device ' + d.id.toString().slice(-8)

function DeviceGroup({ device, devices, s }) {
  const children = devices.filter((x) => x.parentId === device.id)
  const entities = store.entitiesOfDevice(device.id)
  return (
    <div className="device-group">
      <h3 className="device-title">{deviceLabel(device)}</h3>
      {entities.map((e) => <EntityCard key={e.id.toString()} ent={e} />)}
      {children.map((c) => (
        <DeviceGroup key={c.id.toString()} device={c} devices={devices} s={s} />
      ))}
    </div>
  )
}

export default function Entities() {
  const s = useStore()
  const [status, setStatus] = useState('')

  const devices = [...s.semDevices.values()]
  const known = new Set(devices.map((d) => d.id.toString()))
  const roots = devices.filter((d) => d.parentId === 0n || !known.has(d.parentId.toString()))
  roots.sort((a, b) => deviceLabel(a).localeCompare(deviceLabel(b)))

  const unassigned = [...s.semEntities.values()].filter(
    (e) => e.deviceId === 0n || !known.has(e.deviceId.toString()),
  )
  unassigned.sort((a, b) => (a.id < b.id ? -1 : 1))

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
        <DeviceGroup key={d.id.toString()} device={d} devices={devices} s={s} />
      ))}
      {unassigned.length > 0 && (
        <div className="device-group">
          <h3 className="device-title muted">Без устройства</h3>
          {unassigned.map((e) => <EntityCard key={e.id.toString()} ent={e} />)}
        </div>
      )}
    </section>
  )
}
