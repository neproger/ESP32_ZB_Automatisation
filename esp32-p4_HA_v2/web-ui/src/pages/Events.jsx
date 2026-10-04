import { useState } from 'react'
import { useStore } from '../useStore.js'
import { ENTITY } from '../schema.js'
import { uidHex, clusterName, attrName, formatAttrValue } from '../zcl.js'

// Журнал изменений: собирается из WS-дельт (см. store.logEvent). Snapshot не логируется.

function deviceLabel(devices, uid) {
  for (const { key, record } of devices.values()) {
    if (key.uid === uid) return record.name || record.model || uidHex(uid)
  }
  return uidHex(uid)
}

function describe(ev, s) {
  const { entityType, key, record, action } = ev
  switch (entityType) {
    case ENTITY.DEVICE:
      return action === 'remove'
        ? `устройство ${deviceLabel(s.devices, key.uid)} удалено`
        : `устройство ${record.name || record.model || uidHex(key.uid)}`
    case ENTITY.STATE:
      return `${deviceLabel(s.devices, key.uid)} · EP${key.ep} · ${clusterName(key.cluster)} ${attrName(key.cluster, key.attr)} = ${formatAttrValue(key.cluster, key.attr, record.zclType, record.raw)}`
    case ENTITY.ENDPOINT:
      return `топология ${deviceLabel(s.devices, key.uid)} · EP${key.ep}`
    case ENTITY.AUTOMATION:
      return `правило #${key.id}`
    case ENTITY.DEVICE_REMOVE:
      return `на удаление ${uidHex(key.uid)}`
    default:
      return ''
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
              <span className={`kind ${e.action}`}>{e.action === 'remove' ? '−' : '+'}</span>
              <span>{describe(e, s)}</span>
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}
