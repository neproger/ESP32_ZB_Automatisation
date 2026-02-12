import { formatDeviceStateValue, getDeviceStateKeyLabel } from './stateFormat.js'

const ENDPOINT_KIND_LABELS = {
  color_light: 'цветная лампа',
  dimmable_light: 'диммируемый свет',
  relay: 'реле',
  dimmer_switch: 'диммер',
  switch: 'кнопка/выключатель',
  temp_humidity_sensor: 'датчик температуры и влажности',
  temperature_sensor: 'датчик температуры',
  humidity_sensor: 'датчик влажности',
  occupancy_sensor: 'датчик присутствия',
  illuminance_sensor: 'датчик освещенности',
  pressure_sensor: 'датчик давления',
  flow_sensor: 'датчик потока',
  sensor: 'сенсор',
  unknown: 'endpoint',
}

const GATEWAY_EVENT_LABELS = {
  'zigbee.read_attr_resp': 'ответ на чтение атрибута',
  'zigbee.cmd_queue': 'отправка команды',
  'zigbee.cmd_sent': 'команда отправлена',
  'device.changed': 'список устройств обновлен',
  'automation.changed': 'список автоматизаций обновлен',
}

const DEVICE_EVENT_LABELS = {
  command: 'команда устройства',
  join: 'устройство присоединилось',
  leave: 'устройство вышло из сети',
  updated: 'устройство обновлено',
}

const AUTOMATION_EVENT_LABELS = {
  'automation.run': 'запуск автоматизации',
  'automation.action': 'выполнение действия',
  'automation.result': 'результат автоматизации',
  'automation.changed': 'список автоматизаций обновлен',
}

function normalizeUid(v) {
  return String(v ?? '').trim().toLowerCase()
}

function formatHex16(v) {
  const n = Number(v)
  if (!Number.isFinite(n)) return ''
  return `0x${Math.max(0, n).toString(16).toUpperCase().padStart(4, '0')}`
}

function collectCommandDetails(data) {
  const parts = []
  const endpoint = Number(data?.endpoint ?? data?.endpoint_id ?? NaN)
  const cmd = String(data?.cmd ?? data?.command ?? data?.action ?? '').trim()
  const clusterRaw = data?.cluster
  const attrRaw = data?.attr
  const cluster = typeof clusterRaw === 'string' && clusterRaw ? clusterRaw : formatHex16(clusterRaw)
  const attr = typeof attrRaw === 'string' && attrRaw ? attrRaw : formatHex16(attrRaw)
  if (Number.isFinite(endpoint)) parts.push(`ep: ${endpoint}`)
  if (cluster) parts.push(`cluster: ${cluster}`)
  if (attr) parts.push(`attr: ${attr}`)
  if (cmd) parts.push(`cmd: ${cmd}`)
  return parts
}

function endpointKindLabel(kind) {
  const k = String(kind ?? '').trim()
  return ENDPOINT_KIND_LABELS[k] || k || 'endpoint'
}

function formatEndpointLabel(device, endpointId) {
  const epNum = Number(endpointId ?? 0)
  if (!Number.isFinite(epNum)) return 'endpoint'
  const eps = Array.isArray(device?.endpoints) ? device.endpoints : []
  const ep = eps.find((x) => Number(x?.endpoint ?? 0) === epNum)
  const suffix = ep ? endpointKindLabel(ep?.kind) : 'endpoint'
  return `EP${epNum} (${suffix})`
}

function formatGatewayEvent(data) {
  const evType = String(data?.event_type ?? '')
  const label = GATEWAY_EVENT_LABELS[evType] || evType || 'событие шлюза'
  const source = String(data?.source ?? '').trim()
  const shortAddr = Number(data?.short_addr ?? NaN)
  const parts = [label]
  parts.push(...collectCommandDetails(data))
  if (source) parts.push(`источник: ${source}`)
  if (Number.isFinite(shortAddr)) parts.push(`short: 0x${Math.max(0, shortAddr).toString(16).toUpperCase()}`)
  return parts.join(' · ')
}

function formatDeviceEvent(data) {
  const ev = String(data?.event ?? '')
  const label = DEVICE_EVENT_LABELS[ev] || ev || 'событие устройства'
  const source = String(data?.source ?? '').trim()
  const parts = [label, ...collectCommandDetails(data)]
  if (source) parts.push(`источник: ${source}`)
  return parts.join(' · ')
}

function formatAutomationEvent(data) {
  const eventType = String(data?.event_type ?? '')
  const base = AUTOMATION_EVENT_LABELS[eventType] || 'автоматизация'
  const id = String(data?.automation_id ?? '').trim()
  const actionIdx = Number(data?.action_idx ?? NaN)
  const hasOk = typeof data?.ok === 'boolean'
  const okText = hasOk ? (data.ok ? 'успех' : 'ошибка') : ''
  const parts = [base]
  if (id) parts.push(`id: ${id}`)
  if (Number.isFinite(actionIdx)) parts.push(`действие #${actionIdx}`)
  if (okText) parts.push(okText)
  return parts.join(' · ')
}

export function formatEventData(event, options = {}) {
  const data = event?.data && typeof event.data === 'object' ? event.data : {}
  const getDeviceByUid = options?.getDeviceByUid
  const fallback = options?.fallbackText || ((v) => String(v ?? ''))
  const type = String(event?.type ?? '')

  if (type === 'device.state') {
    const uid = normalizeUid(data?.device_id)
    const endpoint = Number(data?.endpoint_id ?? data?.endpoint ?? 0)
    const key = String(data?.key ?? '')
    const prettyValue = formatDeviceStateValue(key, data?.value)
    const keyLabel = getDeviceStateKeyLabel(key)
    const device = typeof getDeviceByUid === 'function' ? getDeviceByUid(uid) : null
    const epLabel = formatEndpointLabel(device, endpoint)
    return `${epLabel} · ${keyLabel}: ${prettyValue}`
  }

  if (type === 'gateway.event') {
    if (data?.automation_id || String(data?.event_type ?? '').startsWith('automation.')) {
      return formatAutomationEvent(data)
    }
    return formatGatewayEvent(data)
  }

  if (type === 'device.event') {
    return formatDeviceEvent(data)
  }

  if (data?.automation_id) {
    return formatAutomationEvent(data)
  }

  return fallback(data)
}
