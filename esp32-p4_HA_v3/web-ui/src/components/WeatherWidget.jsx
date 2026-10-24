import { useStore } from '../useStore.js'
import { WEATHER_DEVICE_UID, weatherConditionName } from '../weather.js'
import { SYSTEM_DEVICE_UID } from '../system.js'

// Header-виджет погоды (как в HA v1): локация + температура/влажность. Читает canonical
// запись WEATHER и имя LOCATION системного устройства — без ZCL.
export default function WeatherWidget() {
  const s = useStore()
  const weather = s.weather.get(`weather:${WEATHER_DEVICE_UID}`)?.record
  const loc = s.locations.get(`loc:${SYSTEM_DEVICE_UID}`)?.record
  const location = loc?.name || 'Локация не задана'
  const line = weather
    ? `${weather.temperatureC.toFixed(1)}°C  ${Math.round(weather.humidityPct)}%`
    : '--.-°C  --%'
  return (
    <div className="weather-header-widget"
      title={weather ? weatherConditionName(weather.condition) : 'Погода'}>
      <div className="weather-location">{location}</div>
      <div className="weather-values">{line}</div>
    </div>
  )
}
