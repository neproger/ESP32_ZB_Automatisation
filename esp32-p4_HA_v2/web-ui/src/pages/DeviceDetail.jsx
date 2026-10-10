import { useEffect, useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { renameDevice, removeDevice, cancelRemoveDevice } from '../proto.js'
import { uidHex } from '../zcl.js'
import EndpointWidgets from '../components/EndpointWidgets.jsx'

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

  const endpoints = [...s.endpoints.values()]
    .filter((e) => e.key.uid === target)
    .sort((a, b) => a.key.ep - b.key.ep)

  return (
    <section>
      <p className="pad"><a href="#/devices">← Устройства</a> · <a href="#/diagnostics">Диагностика</a></p>
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
          {s.isMarkedForRemoval(target) ? (
            <>
              <span className="tag warn">на удаление</span>
              <button onClick={() => store.send(cancelRemoveDevice(target))}>Отменить</button>
            </>
          ) : (
            <button onClick={() => store.send(removeDevice(target))}>Удалить</button>
          )}
        </div>

        <h3>Управление</h3>
        {endpoints.length === 0 ? (
          <p className="muted">топология ещё не собрана</p>
        ) : (
          endpoints.map((e) => (
            <div className="endpoint" key={e.key.ep}>
              <div className="ep-head"><span className="ep-badge">EP{e.key.ep}</span></div>
              <EndpointWidgets uid={target} ep={e.key.ep} />
            </div>
          ))
        )}
      </div>
    </section>
  )
}
