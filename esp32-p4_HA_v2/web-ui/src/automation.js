// Семантическая модель правила автоматизации (docs/PROPERTY_MODEL.md): Property/Action/Event.
// Никакого ZCL: cluster/attr/command id/args здесь нет — кодировкой занимается бэкенд.
import { PROPERTY, ACTION, EVENT, propertyName, actionName, eventName } from './semantics.js'

export { PROPERTY, ACTION, EVENT, propertyName, actionName, eventName }

export const TRIGGER_KINDS = [
  [0, 'Событие устройства'],
  [1, 'Время'],
  [2, 'Состояние'],
]

// Операторы STATE-триггера и условий (ha_condition_op_t).
export const TRIGGER_OPS = [
  [1, '='], [2, '≠'], [3, '>'], [4, '<'], [5, '≥'], [6, '≤'],
]
export const CONDITION_OPS = [
  [1, '='], [2, '≠'], [3, '>'], [4, '<'], [5, '≥'], [6, '≤'], [7, 'содержит биты'], [8, 'диапазон'],
]
export const TRIGGER_EDGES = [
  [0, 'любое изменение'], [1, 'стало истинно'], [2, 'стало ложно'],
]

export const WEEKDAY_LABELS = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс']

// Системные события (представимы для system-девайса). Device-события — по мере support.
export const SYSTEM_EVENT_OPTIONS = [
  [EVENT.MINUTE_TICK, 'каждую минуту'],
  [EVENT.HALF_HOUR_TICK, 'каждые 30 мин'],
  [EVENT.HOUR_TICK, 'каждый час'],
  [EVENT.DAY_TICK, 'начало суток'],
]

// Управляемые свойства и их действия (зеркало zb_action_map в semantics).
export const ACTION_PROPERTIES = [
  { id: PROPERTY.POWER, actions: [ACTION.ON, ACTION.OFF, ACTION.TOGGLE] },
  { id: PROPERTY.BRIGHTNESS, actions: [ACTION.SET], value: 'percent' },
  { id: PROPERTY.COLOR_TEMPERATURE, actions: [ACTION.SET], value: 'kelvin' },
  { id: PROPERTY.COLOR_HUE, actions: [ACTION.SET], value: 'degrees' },
  { id: PROPERTY.COLOR_SATURATION, actions: [ACTION.SET], value: 'percent' },
  { id: PROPERTY.COLOR, actions: [ACTION.SET], value: 'color' },
]

const label = (list, v) => (list.find(([x]) => x === v) || [, '?'])[1]

export function minutesToHHMM(minutes) {
  const m = ((Number(minutes) || 0) + 1440) % 1440
  return `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`
}
export function hhmmToMinutes(text) {
  const [h, m] = String(text || '').split(':').map(Number)
  return ((h || 0) * 60 + (m || 0)) % 1440
}
export function maskToDays(mask) {
  return WEEKDAY_LABELS.filter((_, i) => ((Number(mask) || 0) >> i) & 1).join(', ')
}

const nameOfOr = (uid, nameOf) => (uid && BigInt(uid) !== 0n ? nameOf(uid) : 'триггер')

export function describeTrigger(t, nameOf) {
  if (t.kind === 1) {
    return `Время ${minutesToHHMM(t.minutesOfDay)} · ${maskToDays(t.weekdayMask) || '—'}`
  }
  if (t.kind === 0) {
    const dev = t.deviceUid && BigInt(t.deviceUid) !== 0n ? nameOf(t.deviceUid) : '—'
    return `${dev} · ${eventName(t.eventId)}`
  }
  const dev = t.deviceUid && BigInt(t.deviceUid) !== 0n ? nameOf(t.deviceUid) : '—'
  return `${dev}: ${propertyName(t.property)} ${label(TRIGGER_OPS, t.op)} ${t.value} (${label(TRIGGER_EDGES, t.edge)})`
}

export function describeCondition(c, nameOf) {
  const op = label(CONDITION_OPS, c.op)
  const val = c.op === 8 ? `${c.value}…${c.value2}` : c.value
  return `${nameOfOr(c.deviceUid, nameOf)}: ${propertyName(c.property)} ${op} ${val}`
}

export function describeAction(a, nameOf) {
  const suffix =
    a.valueKind === 1 ? ` ${a.value}` : a.valueKind === 2 ? ` xy ${Number(a.x).toFixed(2)},${Number(a.y).toFixed(2)}` : ''
  return `${nameOfOr(a.deviceUid, nameOf)}: ${propertyName(a.property)} · ${actionName(a.action)}${suffix}`
}
