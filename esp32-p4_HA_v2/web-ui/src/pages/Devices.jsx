import { useStore } from '../useStore.js'
import DeviceCard from '../components/DeviceCard.jsx'

export default function Devices() {
  const store = useStore()
  const devices = [...store.devices.values()]
  return (
    <section>
      <h2>Устройства</h2>
      {devices.length === 0 ? (
        <p className="muted pad">Устройств пока нет</p>
      ) : (
        devices.map((d) => <DeviceCard key={d.key.uid.toString()} dev={d} />)
      )}
    </section>
  )
}
