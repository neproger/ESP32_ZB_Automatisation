// Клиентский журнал событий: собирается из live-дельт (страница «События»).
// Сбрасывается при перезагрузке; live-флаг гасит записи во время начальной синхронизации.

const EVENT_LOG_MAX = 200
const events = []
let seq = 0
let live = false

export function setLive(value) {
  live = value
}

export function pushEvent(ev) {
  if (!live) return
  events.push({ seq: ++seq, ts: Date.now(), ...ev })
  if (events.length > EVENT_LOG_MAX) events.splice(0, events.length - EVENT_LOG_MAX)
}

export function getEvents() {
  return events
}
