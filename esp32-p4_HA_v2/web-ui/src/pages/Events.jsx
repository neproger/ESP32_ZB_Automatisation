import { useState } from 'react'
import { useStore } from '../useStore.js'
import { ENTITY } from '../schema.js'
import { eventName } from '../semantics.js'

// Журнал: семантические события (HA_EVENT_*) + изменения generic-сущностей. Snapshot не
// логируется. Raw Zigbee в Domain больше не попадает (Шаг C), поэтому журнал entity-native.

const TYPE_LABEL = {
  [ENTITY.AUTOMATION]: 'правило',
  [ENTITY.LOCATION]: 'локация',
  [ENTITY.GROUP]: 'экран',
  [ENTITY.WEATHER]: 'погода',
  [ENTITY.WIFI_STATUS]: 'wifi-статус',
  [ENTITY.SETTINGS]: 'настройки',
}

function entityLabel(s, entity) {
  return s.semEntities.get(entity.toString())?.name || entity.toString().slice(-6)
}

function describe(ev, s) {
  if (ev.kind === 'event') {
    return `${entityLabel(s, ev.sourceEntity)}: ${eventName(ev.eventId)}`
  }
  const label = TYPE_LABEL[ev.entityType] || 'сущность'
  return ev.action === 'remove' ? `${label} удалена` : `${label} обновлена`
}

export default function Events() {
  const s = useStore()
  const [paused, setPaused] = useState(false)
  const [frozen, setFrozen] = useState([])
  const [clearedSeq, setClearedSeq] = useState(0)

  const live = s.events.filter((e) => e.seq > clearedSeq)
  const shown = [...(paused ? frozen : live)].reverse()

  const togglePause = () => {
    if (paused) {
      setPaused(false)
    } else {
      setFrozen(live)
      setPaused(true)
    }
  }
  const clear = () => {
    setClearedSeq(live.length ? live[live.length - 1].seq : clearedSeq)
    setFrozen([])
  }

  return (
    <section>
      <h2>
        События
        <button onClick={togglePause}>{paused ? 'Продолжить' : 'Пауза'}</button>
        <button onClick={clear}>Очистить</button>
      </h2>
      {shown.length === 0 ? (
        <p className="muted pad">событий пока нет</p>
      ) : (
        <ul className="events">
          {shown.map((e) => (
            <li key={e.seq}>
              <span className="time">{new Date(e.ts).toLocaleTimeString()}</span>
              <span className={`kind ${e.action}`}>{e.action === 'remove' ? '−' : '+'}</span>
              <span>{describe(e, s)}</span>
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}
