import { useEffect, useState } from 'react'
import { useStore } from './useStore.js'
import Devices from './pages/Devices.jsx'
import DeviceDetail from './pages/DeviceDetail.jsx'
import Automations from './pages/Automations.jsx'
import Events from './pages/Events.jsx'
import SystemStatus from './components/SystemStatus.jsx'

// Ручной роутинг поверх hash: сервер отдаёт один документ, навигация — на клиенте.
function useHash() {
  const [hash, setHash] = useState(() => location.hash || '#/devices')
  useEffect(() => {
    const on = () => setHash(location.hash || '#/devices')
    window.addEventListener('hashchange', on)
    return () => window.removeEventListener('hashchange', on)
  }, [])
  return hash
}

export default function App() {
  const store = useStore()
  const hash = useHash()
  const [page, arg] = hash.replace(/^#\/?/, '').split('/')
  const current = page || 'devices'
  const online = store.status === 'online'

  return (
    <>
      <header>
        <h1>ESP32-P4 HA</h1>
        <nav>
          <a href="#/devices" className={current === 'devices' || current === 'device' ? 'active' : ''}>
            Устройства
          </a>
          <a href="#/events" className={current === 'events' ? 'active' : ''}>
            События
          </a>
          <a href="#/automations" className={current === 'automations' ? 'active' : ''}>
            Автоматизации
          </a>
        </nav>
        <SystemStatus />
        <span className={'link' + (online ? ' ok' : '')}>{online ? 'подключено' : 'нет связи'}</span>
      </header>
      <main>
        {current === 'automations' ? (
          <Automations />
        ) : current === 'events' ? (
          <Events />
        ) : current === 'device' ? (
          <DeviceDetail uid={arg} />
        ) : (
          <Devices />
        )}
      </main>
    </>
  )
}
