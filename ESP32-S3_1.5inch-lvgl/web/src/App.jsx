//UTF-8
//App.jsx
import { useEffect, useState } from 'react'
import { Navigate, NavLink, Route, Routes } from 'react-router-dom'
import './App.css'
import Devices from './pages/Devices.jsx'
import Device from './pages/Device.jsx'
import Events from './pages/Events.jsx'
import Automations from './pages/Automations.jsx'
import { GatewayProvider } from './gateway.jsx'

const THEME_KEY = 'gw_theme'

function detectInitialTheme() {
  const saved = window.localStorage.getItem(THEME_KEY)
  if (saved === 'dark' || saved === 'light') return saved
  return window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark'
}

function App() {
  const [theme, setTheme] = useState(detectInitialTheme)

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme)
    window.localStorage.setItem(THEME_KEY, theme)
  }, [theme])

  const toggleTheme = () => setTheme((cur) => (cur === 'dark' ? 'light' : 'dark'))

  return (
    <GatewayProvider>
      <div className="layout">
        <nav className="nav">
          <div className="brand">Zigbee Gateway</div>
          <NavLink to="/devices" className={({ isActive }) => (isActive ? 'navlink active' : 'navlink')}>
            Devices
          </NavLink>
          <NavLink to="/events" className={({ isActive }) => (isActive ? 'navlink active' : 'navlink')}>
            Events
          </NavLink>
          <NavLink to="/automations" className={({ isActive }) => (isActive ? 'navlink active' : 'navlink')}>
            Automations
          </NavLink>
          <div className="nav-spacer" />
          <button className="theme-toggle" onClick={toggleTheme}>
            {theme === 'dark' ? 'Light theme' : 'Dark theme'}
          </button>
        </nav>

        <main className="content">
          <Routes>
            <Route path="/" element={<Navigate to="/devices" replace />} />
            <Route path="/devices" element={<Devices />} />
            <Route path="/devices/:uid" element={<Device />} />
            <Route path="/events" element={<Events />} />
            <Route path="/automations" element={<Automations />} />
            <Route path="*" element={<Navigate to="/devices" replace />} />
          </Routes>
        </main>
      </div>
    </GatewayProvider>
  )
}

export default App
