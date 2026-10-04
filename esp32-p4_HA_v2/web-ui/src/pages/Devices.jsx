import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { permitJoin } from '../proto.js'
import DeviceCard from '../components/DeviceCard.jsx'

const JOIN_SECONDS = 180

export default function Devices() {
  const s = useStore()
  const [status, setStatus] = useState('')
  const devices = [...s.devices.values()]

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
      {devices.length === 0 ? (
        <p className="muted pad">Устройств пока нет</p>
      ) : (
        devices.map((d) => <DeviceCard key={d.key.uid.toString()} dev={d} />)
      )}
    </section>
  )
}
