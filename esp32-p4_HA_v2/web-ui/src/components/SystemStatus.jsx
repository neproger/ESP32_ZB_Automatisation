import { useStore } from '../useStore.js'
import { SYSTEM_DEVICE_UID, SYSTEM_ENDPOINT, WEEKDAY_NAMES } from '../system.js'
import { PROPERTY } from '../semantics.js'
import { WEATHER_DEVICE_UID, weatherConditionName } from '../weather.js'

// Время/локация/погода: только semantic-состояния системного девайса (без cluster/raw).
export default function SystemStatus() {
  const s = useStore()
  const sem = (p) => s.semState(SYSTEM_DEVICE_UID, SYSTEM_ENDPOINT, p)?.value
  const hour = sem(PROPERTY.SYSTEM_HOUR)
  const minute = sem(PROPERTY.SYSTEM_MINUTE)
  const mask = sem(PROPERTY.SYSTEM_WEEKDAY_MASK)
  const loc = s.locations.get(`loc:${SYSTEM_DEVICE_UID}`)?.record
  const weather = s.weather.get(`weather:${WEATHER_DEVICE_UID}`)?.record

  const time = hour != null && minute != null
    ? `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`
    : '--:--'
  const days = mask != null ? WEEKDAY_NAMES.filter((_, i) => (Number(mask) >> i) & 1).join(' ') : ''

  return (
    <span className="sysstatus" title={loc ? `${loc.latitude.toFixed(3)}, ${loc.longitude.toFixed(3)}` : ''}>
      {loc?.name ? <b>{loc.name}</b> : null}
      <span className="clock">{time}</span>
      {weather ? (
        <span className="weather">
          {weatherConditionName(weather.condition)} {weather.temperatureC.toFixed(1)}°C
        </span>
      ) : null}
      {days ? <span className="muted">{days}</span> : null}
    </span>
  )
}
