import { EVT_AUTOMATION_CHANGED, EVT_DEVICE_STATE } from '../eventNames.js'
import { formatDeviceStateValue, getDeviceStateKeyLabel } from './stateFormat.js'

const ENDPOINT_KIND_LABELS = {
  color_light: 'С†РІРµС‚РЅР°СЏ Р»Р°РјРїР°',
  dimmable_light: 'РґРёРјРјРёСЂСѓРµРјС‹Р№ СЃРІРµС‚',
  relay: 'СЂРµР»Рµ',
  dimmer_switch: 'РґРёРјРјРµСЂ',
  switch: 'РєРЅРѕРїРєР°/РІС‹РєР»СЋС‡Р°С‚РµР»СЊ',
  temp_humidity_sensor: 'РґР°С‚С‡РёРє С‚РµРјРїРµСЂР°С‚СѓСЂС‹ Рё РІР»Р°Р¶РЅРѕСЃС‚Рё',
  temperature_sensor: 'РґР°С‚С‡РёРє С‚РµРјРїРµСЂР°С‚СѓСЂС‹',
  humidity_sensor: 'РґР°С‚С‡РёРє РІР»Р°Р¶РЅРѕСЃС‚Рё',
  occupancy_sensor: 'РґР°С‚С‡РёРє РїСЂРёСЃСѓС‚СЃС‚РІРёСЏ',
  illuminance_sensor: 'РґР°С‚С‡РёРє РѕСЃРІРµС‰РµРЅРЅРѕСЃС‚Рё',
  pressure_sensor: 'РґР°С‚С‡РёРє РґР°РІР»РµРЅРёСЏ',
  flow_sensor: 'РґР°С‚С‡РёРє РїРѕС‚РѕРєР°',
  sensor: 'СЃРµРЅСЃРѕСЂ',
  unknown: 'endpoint',
}

const GATEWAY_EVENT_LABELS = {
  'zigbee.read_attr_resp': 'РѕС‚РІРµС‚ РЅР° С‡С‚РµРЅРёРµ Р°С‚СЂРёР±СѓС‚Р°',
  'zigbee.cmd_queue': 'РѕС‚РїСЂР°РІРєР° РєРѕРјР°РЅРґС‹',
  'zigbee.cmd_sent': 'РєРѕРјР°РЅРґР° РѕС‚РїСЂР°РІР»РµРЅР°',
  [EVT_AUTOMATION_CHANGED]: 'СЃРїРёСЃРѕРє Р°РІС‚РѕРјР°С‚РёР·Р°С†РёР№ РѕР±РЅРѕРІР»РµРЅ',
  'proto.sync_begin': 'РЅР°С‡Р°Р»Рѕ СЃРёРЅС…СЂРѕРЅРёР·Р°С†РёРё',
  'proto.sync_end': 'РєРѕРЅРµС† СЃРёРЅС…СЂРѕРЅРёР·Р°С†РёРё',
}

const DEVICE_EVENT_LABELS = {
  command: 'РєРѕРјР°РЅРґР° СѓСЃС‚СЂРѕР№СЃС‚РІР°',
  join: 'СѓСЃС‚СЂРѕР№СЃС‚РІРѕ РїСЂРёСЃРѕРµРґРёРЅРёР»РѕСЃСЊ',
  leave: 'СѓСЃС‚СЂРѕР№СЃС‚РІРѕ РІС‹С€Р»Рѕ РёР· СЃРµС‚Рё',
  updated: 'СѓСЃС‚СЂРѕР№СЃС‚РІРѕ РѕР±РЅРѕРІР»РµРЅРѕ',
}

const AUTOMATION_EVENT_LABELS = {
  'automation.run': 'Р·Р°РїСѓСЃРє Р°РІС‚РѕРјР°С‚РёР·Р°С†РёРё',
  'automation.action': 'РІС‹РїРѕР»РЅРµРЅРёРµ РґРµР№СЃС‚РІРёСЏ',
  'automation.result': 'СЂРµР·СѓР»СЊС‚Р°С‚ Р°РІС‚РѕРјР°С‚РёР·Р°С†РёРё',
  [EVT_AUTOMATION_CHANGED]: 'СЃРїРёСЃРѕРє Р°РІС‚РѕРјР°С‚РёР·Р°С†РёР№ РѕР±РЅРѕРІР»РµРЅ',
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
  const label = GATEWAY_EVENT_LABELS[evType] || evType || 'СЃРѕР±С‹С‚РёРµ С€Р»СЋР·Р°'
  const source = String(data?.source ?? '').trim()
  const shortAddr = Number(data?.short_addr ?? NaN)
  const parts = [label]
  parts.push(...collectCommandDetails(data))
  if (source) parts.push(`РёСЃС‚РѕС‡РЅРёРє: ${source}`)
  if (Number.isFinite(shortAddr)) parts.push(`short: 0x${Math.max(0, shortAddr).toString(16).toUpperCase()}`)
  return parts.join(' В· ')
}

function formatDeviceEvent(data) {
  const ev = String(data?.event ?? '')
  const label = DEVICE_EVENT_LABELS[ev] || ev || 'СЃРѕР±С‹С‚РёРµ СѓСЃС‚СЂРѕР№СЃС‚РІР°'
  const source = String(data?.source ?? '').trim()
  const parts = [label, ...collectCommandDetails(data)]
  if (source) parts.push(`РёСЃС‚РѕС‡РЅРёРє: ${source}`)
  return parts.join(' В· ')
}

function formatAutomationEvent(data) {
  const eventType = String(data?.event_type ?? '')
  const base = AUTOMATION_EVENT_LABELS[eventType] || 'Р°РІС‚РѕРјР°С‚РёР·Р°С†РёСЏ'
  const id = String(data?.automation_id ?? '').trim()
  const actionIdx = Number(data?.action_idx ?? NaN)
  const hasOk = typeof data?.ok === 'boolean'
  const okText = hasOk ? (data.ok ? 'СѓСЃРїРµС…' : 'РѕС€РёР±РєР°') : ''
  const parts = [base]
  if (id) parts.push(`id: ${id}`)
  if (Number.isFinite(actionIdx)) parts.push(`РґРµР№СЃС‚РІРёРµ #${actionIdx}`)
  if (okText) parts.push(okText)
  return parts.join(' В· ')
}

export function formatEventData(event, options = {}) {
  const data = event?.data && typeof event.data === 'object' ? event.data : {}
  const getDeviceByUid = options?.getDeviceByUid
  const fallback = options?.fallbackText || ((v) => String(v ?? ''))
  const type = String(event?.type ?? '')

  if (type === EVT_DEVICE_STATE) {
    const uid = normalizeUid(data?.device_id)
    const endpoint = Number(data?.endpoint_id ?? data?.endpoint ?? 0)
    const key = String(data?.key ?? '')
    const prettyValue = formatDeviceStateValue(key, data?.value)
    const keyLabel = getDeviceStateKeyLabel(key)
    const device = typeof getDeviceByUid === 'function' ? getDeviceByUid(uid) : null
    const epLabel = formatEndpointLabel(device, endpoint)
    return `${epLabel} В· ${keyLabel}: ${prettyValue}`
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
