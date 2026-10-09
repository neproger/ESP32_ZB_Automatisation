import { useEffect, useMemo, useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { locationPut } from '../proto.js'
import { SYSTEM_DEVICE_UID } from '../system.js'

function tzLabel(min) {
  const v = Number(min) || 0
  const sign = v >= 0 ? '+' : '-'
  const a = Math.abs(v)
  return `UTC${sign}${String(Math.floor(a / 60)).padStart(2, '0')}:${String(a % 60).padStart(2, '0')}`
}

// Локация и часовой пояс (docs/services/SYSTEM.md), как в v1: tz — авто/смещение;
// место — авто (GeoIP) или выбор города через Open-Meteo geocoding в браузере.
export default function Settings() {
  const s = useStore()
  const loc = s.locations.get('loc:' + SYSTEM_DEVICE_UID)?.record

  const [timezone, setTimezone] = useState('auto')
  const [posAuto, setPosAuto] = useState(true)
  const [name, setName] = useState('')
  const [lat, setLat] = useState(0)
  const [lon, setLon] = useState(0)
  const [citySearch, setCitySearch] = useState('')
  const [cityResults, setCityResults] = useState([])
  const [searching, setSearching] = useState(false)
  const [saved, setSaved] = useState(false)

  const tzOptions = useMemo(() => {
    const out = [{ value: 'auto', label: 'Авто (по региону)' }]
    for (let m = -12 * 60; m <= 14 * 60; m += 30) {
      out.push({ value: String(m), label: tzLabel(m) })
    }
    return out
  }, [])

  useEffect(() => {
    if (!loc) return
    setTimezone(loc.tzAuto ? 'auto' : String(loc.tzOffsetMin ?? 0))
    setPosAuto(!!loc.posAuto)
    setName(loc.name || '')
    setLat(loc.latitude ?? 0)
    setLon(loc.longitude ?? 0)
  }, [loc])

  async function searchCity() {
    const q = citySearch.trim()
    if (!q) return
    setSearching(true)
    try {
      const url = `https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(q)}&count=5&language=ru&format=json`
      const res = await fetch(url)
      const data = await res.json()
      setCityResults(
        (data?.results || []).map((c) => ({
          name: c.name,
          lat: c.latitude,
          lon: c.longitude,
          label: `${c.name}${c.admin1 ? ', ' + c.admin1 : ''}, ${c.country || ''}`,
        })),
      )
    } catch {
      setCityResults([])
    } finally {
      setSearching(false)
    }
  }

  function selectCity(c) {
    setName(c.name)
    setLat(c.lat)
    setLon(c.lon)
    setPosAuto(false)
    setCitySearch('')
    setCityResults([])
  }

  const save = () => {
    const tzAuto = timezone === 'auto'
    store.send(
      locationPut(SYSTEM_DEVICE_UID, {
        posAuto,
        name: name.trim(),
        latitude: Number(lat) || 0,
        longitude: Number(lon) || 0,
        tzAuto,
        tzOffsetMin: tzAuto ? 0 : Math.round(Number(timezone) || 0),
      }),
    )
    setSaved(true)
    setTimeout(() => setSaved(false), 1500)
  }

  return (
    <section className="settings">
      <h2>Настройки</h2>

      <div className="card settings-card">
        <div className="settings-sec">
          <div className="settings-label">Часовой пояс</div>
          <div className="settings-ctl">
            <select value={timezone} onChange={(e) => setTimezone(e.target.value)}>
              {tzOptions.map((o) => (
                <option key={o.value} value={o.value}>{o.label}</option>
              ))}
            </select>
            <span className="muted">
              {timezone === 'auto' ? 'определяется по сети' : tzLabel(timezone)}
            </span>
          </div>
        </div>

        <div className="settings-sec">
          <div className="settings-label">Место</div>
          <div className="settings-ctl col">
            <label className="switch-row">
              <span>Авто (GeoIP)</span>
              <label className="switch">
                <input type="checkbox" checked={posAuto} onChange={(e) => setPosAuto(e.target.checked)} />
                <span />
              </label>
            </label>

            {!posAuto && (
              <div className="city-picker">
                <div className="city-search">
                  <input
                    type="text"
                    placeholder="Поиск города…"
                    value={citySearch}
                    onChange={(e) => setCitySearch(e.target.value)}
                    onKeyDown={(e) => e.key === 'Enter' && searchCity()}
                  />
                  <button type="button" onClick={searchCity} disabled={searching}>
                    {searching ? '…' : 'Найти'}
                  </button>
                </div>
                {cityResults.length > 0 && (
                  <ul className="city-list">
                    {cityResults.map((c, i) => (
                      <li key={i} onClick={() => selectCity(c)}>{c.label}</li>
                    ))}
                  </ul>
                )}
                {name && cityResults.length === 0 && (
                  <div className="city-selected">
                    <span className="dot" />
                    <b>{name}</b>
                    <span className="muted">({Number(lat).toFixed(4)}, {Number(lon).toFixed(4)})</span>
                  </div>
                )}
              </div>
            )}
          </div>
        </div>
      </div>

      <div className="settings-actions">
        <button className="primary" onClick={save} disabled={!loc}>Сохранить</button>
        {saved && <span className="saved">сохранено</span>}
        <span className="muted">
          {loc
            ? `сейчас: ${loc.name || '—'} · ${tzLabel(loc.tzOffsetMin)}`
            : 'локация ещё не определена'}
        </span>
      </div>
    </section>
  )
}
