// Модель правила автоматизации: кластеры/команды и кодирование action_args
// (ZCL-аргументы; прошивка копирует их в команду — automation_rule_command).

import { rgbHexToXy, xyToRgbHex } from './color.js'
import { clusterName, attrName } from './zcl.js'

const u16 = (v) => {
  const n = Math.max(0, Math.min(0xffff, Math.round(v))) & 0xffff
  return [n & 0xff, n >> 8]
}
const readU16 = (lo = 0, hi = 0) => (lo & 0xff) | ((hi & 0xff) << 8)

export const ACTION_CLUSTERS = [
  { id: 0x0006, name: 'On/Off', cmds: [[0, 'Выключить'], [1, 'Включить'], [2, 'Toggle']] },
  { id: 0x0008, name: 'Level', cmds: [[0, 'Перейти к уровню']] },
  { id: 0x0300, name: 'Color', cmds: [[1, 'Температура цвета'], [0, 'Цвет XY']] },
]

// Триггер: 0 = любая команда (прошивка трактует 0 как «любая»).
export const TRIGGER_CMDS = [[0, 'Любая'], [2, 'Toggle'], [1, 'Вкл']]

// Виды триггера (ha_automation_trigger_kind_t).
export const TRIGGER_KINDS = [
  [0, 'Событие устройства'],
  [1, 'Время'],
  [2, 'Состояние'],
]

// Операторы STATE-триггера (те же, кроме «содержит биты»).
export const TRIGGER_OPS = [
  [1, '='],
  [2, '≠'],
  [3, '>'],
  [4, '<'],
  [5, '≥'],
  [6, '≤'],
]

// Когда срабатывать (ha_automation_trigger_edge_t).
export const TRIGGER_EDGES = [
  [0, 'любое изменение'],
  [1, 'стало истинно'],
  [2, 'стало ложно'],
]

export function describeStateTrigger(record, nameOf) {
  const dev = record.triggerUid && BigInt(record.triggerUid) !== 0n ? nameOf(record.triggerUid) : '—'
  const op = (TRIGGER_OPS.find(([v]) => v === record.triggerOp) || [, '?'])[1]
  const isBool = record.triggerCluster === 0x0006 && record.triggerAttr === 0x0000
  const val = isBool ? (record.triggerValue ? 'Вкл' : 'Выкл') : record.triggerValue
  const edge = (TRIGGER_EDGES.find(([v]) => v === record.triggerEdge) || [, ''])[1]
  return `${dev}: ${clusterName(record.triggerCluster)}·${attrName(record.triggerCluster, record.triggerAttr)} ${op} ${val} (${edge})`
}

export const WEEKDAY_LABELS = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс']

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

// Оператор условия: те же шесть, что и в правилах v1 (ha_condition_op_t).
export const CONDITION_OPS = [
  [1, '='],
  [2, '≠'],
  [3, '>'],
  [4, '<'],
  [5, '≥'],
  [6, '≤'],
  [7, 'содержит биты'],
  [8, 'диапазон'],
]

export function describeCondition(c, nameOf) {
  const dev = c.deviceUid && BigInt(c.deviceUid) !== 0n ? nameOf(c.deviceUid) : 'триггер'
  const op = (CONDITION_OPS.find(([v]) => v === c.op) || [, '?'])[1]
  const isBool = c.cluster === 0x0006 && c.attr === 0x0000
  const val =
    c.op === 8
      ? `${c.value}…${c.value2}`
      : isBool
        ? c.value
          ? 'Вкл'
          : 'Выкл'
        : c.value
  return `${dev}: ${clusterName(c.cluster)}·${attrName(c.cluster, c.attr)} ${op} ${val}`
}

export function buildActionArgs(cluster, cmd, p) {
  if (cluster === 0x0006) return []
  if (cluster === 0x0008) return [Math.max(0, Math.min(254, Math.round(p.level || 0))), ...u16(p.transitionMs || 0)]
  if (cluster === 0x0300 && cmd === 1) {
    const k = Math.max(2000, Math.min(6500, p.kelvin || 3000))
    return [...u16(1_000_000 / k), ...u16(p.transitionMs || 0)]
  }
  if (cluster === 0x0300 && cmd === 0) {
    const { x, y } = rgbHexToXy(p.colorHex || '#ffffff')
    return [...u16(x), ...u16(y), ...u16(p.transitionMs || 0)]
  }
  return []
}

export function decodeActionArgs(cluster, cmd, args = []) {
  if (cluster === 0x0008) return { level: args[0] ?? 0, transitionMs: readU16(args[1], args[2]) }
  if (cluster === 0x0300 && cmd === 1) {
    const mired = readU16(args[0], args[1])
    return { kelvin: mired > 0 ? Math.round(1_000_000 / mired) : 3000, transitionMs: readU16(args[2], args[3]) }
  }
  if (cluster === 0x0300 && cmd === 0) {
    return {
      colorHex: xyToRgbHex(readU16(args[0], args[1]), readU16(args[2], args[3])),
      transitionMs: readU16(args[4], args[5]),
    }
  }
  return {}
}

export function describeActionArgs(cluster, cmd, args = []) {
  if (cluster === 0x0008) return `Level=${args[0] ?? 0}${readU16(args[1], args[2]) ? `, ${readU16(args[1], args[2])}ms` : ''}`
  if (cluster === 0x0300 && cmd === 1) {
    const m = readU16(args[0], args[1])
    return m > 0 ? `~${Math.round(1_000_000 / m)}K` : ''
  }
  if (cluster === 0x0300 && cmd === 0) return `xy ${readU16(args[0], args[1])},${readU16(args[2], args[3])}`
  return ''
}
