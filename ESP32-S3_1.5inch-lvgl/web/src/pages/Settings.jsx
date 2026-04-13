import { useEffect, useMemo, useState } from 'react'
import { sendWsCommand } from '../wsCommandBus.js'
import { protoEncodeSettingsChange } from '../proto.js'
import { useGateway } from '../gateway.jsx'

function toInt(v, fallback = 0) {
  const n = Number(v)
  if (!Number.isFinite(n)) return fallback
  return Math.trunc(n)
}

function clampInt(v, min, max, fallback) {
  const n = toInt(v, fallback)
  return Math.max(min, Math.min(max, n))
}

export default function Settings() {
  const { projectSettings, factoryReset } = useGateway()
  const [status, setStatus] = useState('')
  const [saving, setSaving] = useState(false)
  const [citySearch, setCitySearch] = useState('')
  const [cityResults, setCityResults] = useState([])
  const [searching, setSearching] = useState(false)
  const [form, setForm] = useState({
    screensaver_timeout_sec: 4,
    weather_success_interval_min: 60,
    weather_retry_interval_sec: 10,
    timezone: 'auto',
    weather_location_auto: true,
    weather_city: '',
    weather_lat: 0,
    weather_lon: 0,
  })

  async function searchCity() {
    if (!citySearch.trim()) return
    setSearching(true)
    try {
      const url = `https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(citySearch)}&count=5&language=en&format=json`
      const res = await fetch(url)
      const data = await res.json()
      if (data?.results) {
        setCityResults(data.results.map(c => ({
          name: c.name,
          country: c.country || '',
          state: c.admin1 || '',
          lat: c.latitude,
          lon: c.longitude,
          label: `${c.name}${c.admin1 ? ', ' + c.admin1 : ''}, ${c.country || ''}`,
        })))
      }
    } catch (e) {
      console.error('city search failed', e)
    } finally {
      setSearching(false)
    }
  }

  function selectCity(city) {
    setForm(prev => ({
      ...prev,
      weather_location_auto: false,
      weather_city: city.name,
      weather_lat: city.lat,
      weather_lon: city.lon,
    }))
    setCitySearch('')
    setCityResults([])
  }

  const timezoneOptions = useMemo(() => {
    const out = [{ value: 'auto', label: 'Auto (region)' }]
    for (let h = -12; h <= 14; h += 1) {
      const sign = h >= 0 ? '+' : '-'
      const abs = Math.abs(h)
      out.push({ value: String(h), label: `UTC${sign}${abs}` })
    }
    return out
  }, [])

  useEffect(() => {
    if (!projectSettings) return
    const tzAuto = Boolean(projectSettings?.timezone_auto)
    const tzOffsetMin = toInt(projectSettings?.timezone_offset_min, 0)
    const tzHour = Math.trunc(tzOffsetMin / 60)
    setForm({
      screensaver_timeout_sec: Math.max(1, Math.round(toInt(projectSettings?.screensaver_timeout_ms, 4000) / 1000)),
      weather_success_interval_min: Math.max(1, Math.round(toInt(projectSettings?.weather_success_interval_ms, 60 * 60 * 1000) / 60000)),
      weather_retry_interval_sec: Math.max(3, Math.round(toInt(projectSettings?.weather_retry_interval_ms, 10 * 1000) / 1000)),
      timezone: tzAuto ? 'auto' : String(tzHour),
      weather_location_auto: projectSettings?.weather_location_auto !== 0,
      weather_city: projectSettings?.weather_city || '',
      weather_lat: Number(projectSettings?.weather_lat) || 0,
      weather_lon: Number(projectSettings?.weather_lon) || 0,
    })
  }, [projectSettings])

  async function onSave() {
    setSaving(true)
    setStatus('')
    const tzAuto = form.timezone === 'auto'
    const tzHour = clampInt(form.timezone, -12, 14, 0)
    const nextSettings = {
      screensaver_timeout_ms: clampInt(form.screensaver_timeout_sec, 1, 600, 4) * 1000,
      weather_success_interval_ms: clampInt(form.weather_success_interval_min, 1, 1440, 60) * 60 * 1000,
      weather_retry_interval_ms: clampInt(form.weather_retry_interval_sec, 3, 600, 10) * 1000,
      timezone_auto: tzAuto,
      timezone_offset_min: tzAuto ? 0 : tzHour * 60,
      weather_location_auto: form.weather_location_auto,
      weather_lat: form.weather_lat,
      weather_lon: form.weather_lon,
      weather_city: form.weather_city,
    }
    try {
      sendWsCommand(protoEncodeSettingsChange(nextSettings, Date.now() & 0xffff))
      setStatus('Settings saved')
    } catch (e) {
      setStatus(String(e?.message ?? e))
    } finally {
      setSaving(false)
    }
  }

  async function onFactoryReset() {
    const ok = window.confirm('Erase gw_data on S3 and C6, then reboot both devices?')
    if (!ok) return
    setStatus('')
    try {
      await factoryReset()
      setStatus('Factory reset requested. Devices will reboot...')
    } catch (e) {
      setStatus(String(e?.message ?? e))
    }
  }

  return (
    <div className="page">
      <div className="header">
        <div>
          <h1>Settings</h1>
          <div className="muted">Centralized runtime/project settings (RAM cache + persistent NVS).</div>
        </div>
      </div>

      {status ? <div className="status">{status}</div> : null}

      <div className="card settings-card">
        <label className="settings-row">
          <span>Screen saver timeout (sec)</span>
          <input
            type="number"
            min={1}
            max={600}
            value={form.screensaver_timeout_sec}
            onChange={(e) => setForm((prev) => ({ ...prev, screensaver_timeout_sec: toInt(e?.target?.value, prev.screensaver_timeout_sec) }))}
          />
        </label>

        <label className="settings-row">
          <span>Weather update interval (min)</span>
          <input
            type="number"
            min={1}
            max={1440}
            value={form.weather_success_interval_min}
            onChange={(e) => setForm((prev) => ({ ...prev, weather_success_interval_min: toInt(e?.target?.value, prev.weather_success_interval_min) }))}
          />
        </label>

        <label className="settings-row">
          <span>Weather retry interval (sec)</span>
          <input
            type="number"
            min={3}
            max={600}
            value={form.weather_retry_interval_sec}
            onChange={(e) => setForm((prev) => ({ ...prev, weather_retry_interval_sec: toInt(e?.target?.value, prev.weather_retry_interval_sec) }))}
          />
        </label>

        <label className="settings-row">
          <span>Timezone</span>
          <select
            value={form.timezone}
            onChange={(e) => setForm((prev) => ({ ...prev, timezone: String(e?.target?.value ?? 'auto') }))}
          >
            {timezoneOptions.map((tz) => (
              <option key={tz.value} value={tz.value}>
                {tz.label}
              </option>
            ))}
          </select>
        </label>

        <div className="settings-row">
          <span>Weather location</span>
          <div style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
            <label style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
              <input
                type="checkbox"
                checked={form.weather_location_auto}
                onChange={(e) => setForm((prev) => ({ ...prev, weather_location_auto: e.target.checked }))}
              />
              Auto (from IP)
            </label>
            {!form.weather_location_auto && (
              <>
                <div style={{ display: 'flex', gap: '8px' }}>
                  <input
                    type="text"
                    placeholder="Search city..."
                    value={citySearch}
                    onChange={(e) => setCitySearch(e.target.value)}
                    onKeyDown={(e) => e.key === 'Enter' && searchCity()}
                    style={{ flex: 1 }}
                  />
                  <button onClick={searchCity} disabled={searching}>
                    {searching ? '...' : 'Search'}
                  </button>
                </div>
                {cityResults.length > 0 && (
                  <ul style={{ listStyle: 'none', padding: 0, margin: 0, border: '1px solid #ccc' }}>
                    {cityResults.map((c, i) => (
                      <li key={i} style={{ padding: '4px 8px', cursor: 'pointer' }} onClick={() => selectCity(c)}>
                        {c.label}
                      </li>
                    ))}
                  </ul>
                )}
                {form.weather_city && !cityResults.length && (
                  <div style={{ fontSize: '12px', color: '#666' }}>
                    Selected: <strong>{form.weather_city}</strong> ({form.weather_lat?.toFixed(4)}, {form.weather_lon?.toFixed(4)})
                  </div>
                )}
              </>
            )}
          </div>
        </div>

        <div className="row">
          <button onClick={onSave} disabled={saving}>
            {saving ? 'Saving...' : 'Save settings'}
          </button>
        </div>
        <div className="row">
          <button onClick={onFactoryReset}>
            Factory reset
          </button>
        </div>
      </div>
    </div>
  )
}
