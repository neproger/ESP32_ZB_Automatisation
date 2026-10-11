import { useState } from 'react'
import { useStore } from '../useStore.js'
import { eventName, propertyName, propertyUnit } from '../semantics.js'
import { ENTITY_SYSTEM } from '../system.js'

// Журнал: канонические записи (typed). Snapshot не логируется. Никаких числовых Domain-
// типов/сырых записей — каждая запись несёт готовый kind и канонические поля.

function entityLabel(s, entity) {
  if (BigInt(entity) === ENTITY_SYSTEM) return 'Система'
  return s.semEntities.get(entity.toString())?.name || entity.toString().slice(-6)
}

function formatValue(value, property) {
  if (typeof value === 'boolean') return value ? 'да' : 'нет'
  if (value == null) return '—'
  const unit = property != null ? propertyUnit(property) : ''
  return `${Number(value).toFixed(1)}${unit ? ' ' + unit : ''}`
}

function describe(ev, s) {
  switch (ev.kind) {
    case 'event': {
      const base = `${entityLabel(s, ev.entity)}: ${eventName(ev.eventId)}`
      return ev.valueKind && ev.valueKind !== 0 ? `${base} (${ev.value})` : base
    }
    case 'state': {
      const who = entityLabel(s, ev.entity)
      if (ev.removed) return `${who}: ${propertyName(ev.property)} — сброшено`
      return `${who}: ${propertyName(ev.property)} = ${formatValue(ev.value, ev.property)}`
    }
    case 'location':
      return `локация${ev.name ? ` «${ev.name}»` : ''}`
    case 'weather':
      return 'погода'
    case 'wifi':
      return 'wifi-статус'
    case 'settings':
      return 'настройки'
    case 'group':
      return `экран${ev.name ? ` «${ev.name}»` : ` #${ev.id}`}`
    default:
      return '—'
  }
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
              <span className={`kind ${e.kind}`}>{e.removed ? '−' : '+'}</span>
              <span>{describe(e, s)}</span>
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}
