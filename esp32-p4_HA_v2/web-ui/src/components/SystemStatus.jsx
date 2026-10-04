import { useStore } from '../useStore.js'
import { SYSTEM_DEVICE_UID, SYS_ATTR, WEEKDAY_NAMES, systemStateId } from '../system.js'

// Время и локация системного девайса в шапке (состояния приходят как обычные дельты).
export default function SystemStatus() {
  const s = useStore()
  const hour = s.states.get(systemStateId(SYS_ATTR.HOUR))?.record.raw
  const minute = s.states.get(systemStateId(SYS_ATTR.MINUTE))?.record.raw
  const mask = s.states.get(systemStateId(SYS_ATTR.WEEKDAY_MASK))?.record.raw
  const loc = s.locations.get(`loc:${SYSTEM_DEVICE_UID}`)?.record

  const time = hour != null && minute != null
    ? `${String(hour & 0xff).padStart(2, '0')}:${String(minute & 0xff).padStart(2, '0')}`
    : '--:--'
  const days = mask != null ? WEEKDAY_NAMES.filter((_, i) => ((mask >>> 0) >> i) & 1).join(' ') : ''

  return (
    <span className="sysstatus" title={loc ? `${loc.latitude.toFixed(3)}, ${loc.longitude.toFixed(3)}` : ''}>
      {loc?.name ? <b>{loc.name}</b> : null}
      <span className="clock">{time}</span>
      {days ? <span className="muted">{days}</span> : null}
    </span>
  )
}
