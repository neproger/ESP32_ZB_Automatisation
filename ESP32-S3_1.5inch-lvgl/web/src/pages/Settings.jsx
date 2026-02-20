import { useEffect, useMemo, useState } from 'react'
import { postCbor } from '../api.js'
import { useGateway } from '../gateway.jsx'

function toInt(v, fallback = 0) {
  const n = Number(v)
  if (!Number.isFinite(n)) return fallback
  return Math.trunc(n)
}

export default function Settings() {
  const { projectSettings, reloadSettings } = useGateway()
  const [status, setStatus] = useState('')
  const [saving, setSaving] = useState(false)
  const [form, setForm] = useState({
    screensaver_timeout_sec: 4,
    weather_success_interval_min: 60,
    weather_retry_interval_sec: 10,
    timezone: 'auto',
  })

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
    })
  }, [projectSettings])

  async function onSave() {
    setSaving(true)
    setStatus('')
    const tzAuto = form.timezone === 'auto'
    const tzHour = toInt(form.timezone, 0)
    try {
      await postCbor('/api/settings', {
        screensaver_timeout_ms: toInt(form.screensaver_timeout_sec, 4) * 1000,
        weather_success_interval_ms: toInt(form.weather_success_interval_min, 60) * 60 * 1000,
        weather_retry_interval_ms: toInt(form.weather_retry_interval_sec, 10) * 1000,
        timezone_auto: tzAuto,
        timezone_offset_min: tzAuto ? 0 : tzHour * 60,
      })
      await reloadSettings()
      setStatus('Settings saved')
    } catch (e) {
      setStatus(String(e?.message ?? e))
    } finally {
      setSaving(false)
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

        <div className="row">
          <button onClick={onSave} disabled={saving}>
            {saving ? 'Saving...' : 'Save settings'}
          </button>
        </div>
      </div>
    </div>
  )
}
