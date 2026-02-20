import { useMemo } from 'react'
import { useGateway } from '../gateway.jsx'

const WEATHER_UID = '0xweather000000001'
const WEATHER_EP = '1'

function asNum(v) {
  const n = Number(v)
  return Number.isFinite(n) ? n : null
}

export default function WeatherHeaderWidget() {
  const { deviceStates } = useGateway()

  const weather = useMemo(() => {
    const state = deviceStates?.[WEATHER_UID]?.[WEATHER_EP] || {}
    return {
      location: String(state?.weather_location ?? '').trim(),
      tempC: asNum(state?.weather_temp_c),
      humidity: asNum(state?.weather_humidity_pct),
    }
  }, [deviceStates])

  const weatherLine =
    weather.tempC == null || weather.humidity == null
      ? '--.-C  --%'
      : `${weather.tempC.toFixed(1)}C  ${Math.round(weather.humidity)}%`
  const location = weather.location || 'Unknown location'

  return (
    <div className="weather-header-widget" title="Weather">
      <div className="weather-location">{location}</div>
      <div className="weather-values">{weatherLine}</div>
    </div>
  )
}
