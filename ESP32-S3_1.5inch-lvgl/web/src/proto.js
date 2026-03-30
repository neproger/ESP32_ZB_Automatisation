export const GW_PROTO_VERSION_V1 = 1

export const GW_PROTO_MSG_SYNC_BEGIN = 0x40
export const GW_PROTO_MSG_SYNC_END = 0x41
export const GW_PROTO_MSG_DEVICE_UPSERT = 0x42
export const GW_PROTO_MSG_DEVICE_REMOVE = 0x43
export const GW_PROTO_MSG_ENDPOINT_UPSERT = 0x44
export const GW_PROTO_MSG_ENDPOINT_REMOVE = 0x45
export const GW_PROTO_MSG_STATE_ITEM = 0x46
export const GW_PROTO_MSG_STATE_REMOVE = 0x47
export const GW_PROTO_MSG_SETTINGS = 0x4c
export const GW_PROTO_MSG_SNAPSHOT_REQUEST = 0x4d
export const GW_PROTO_MSG_AUTOMATION_UPSERT = 0x4e
export const GW_PROTO_MSG_AUTOMATION_REMOVE = 0x4f

export const GW_PROTO_SYNC_SCOPE_FULL = 1
export const GW_PROTO_SYNC_SCOPE_DEVICES = 2
export const GW_PROTO_SYNC_SCOPE_GROUPS = 3
export const GW_PROTO_SYNC_SCOPE_SETTINGS = 4
export const GW_PROTO_SYNC_SCOPE_AUTOMATIONS = 5

const HDR_SIZE = 8
const DEVICE_UID_SIZE = 19
const DEVICE_NAME_SIZE = 32
const STATE_KEY_SIZE = 24
const STATE_TEXT_SIZE = 64
const STATE_REMOVE_KEY_OFF = DEVICE_UID_SIZE + 1
const DEVICE_SHORT_ADDR_OFF = DEVICE_UID_SIZE
const DEVICE_NAME_OFF = DEVICE_SHORT_ADDR_OFF + 2
const DEVICE_VERSION_OFF = DEVICE_NAME_OFF + DEVICE_NAME_SIZE
const DEVICE_LAST_SEEN_OFF = DEVICE_VERSION_OFF + 4
const DEVICE_HAS_ONOFF_OFF = DEVICE_LAST_SEEN_OFF + 8
const DEVICE_HAS_BUTTON_OFF = DEVICE_HAS_ONOFF_OFF + 1
const ENDPOINT_SHORT_ADDR_OFF = DEVICE_UID_SIZE
const ENDPOINT_ID_OFF = ENDPOINT_SHORT_ADDR_OFF + 2
const ENDPOINT_VERSION_OFF = ENDPOINT_ID_OFF + 2
const ENDPOINT_PROFILE_ID_OFF = ENDPOINT_VERSION_OFF + 4
const ENDPOINT_DEVICE_ID_OFF = ENDPOINT_PROFILE_ID_OFF + 2
const ENDPOINT_IN_CLUSTER_COUNT_OFF = ENDPOINT_DEVICE_ID_OFF + 2
const ENDPOINT_OUT_CLUSTER_COUNT_OFF = ENDPOINT_IN_CLUSTER_COUNT_OFF + 1
const ENDPOINT_IN_CLUSTERS_OFF = ENDPOINT_OUT_CLUSTER_COUNT_OFF + 1
const ENDPOINT_OUT_CLUSTERS_OFF = ENDPOINT_IN_CLUSTERS_OFF + 32
const STATE_ENDPOINT_OFF = DEVICE_UID_SIZE
const STATE_VALUE_TYPE_OFF = STATE_ENDPOINT_OFF + 1
const STATE_KEY_OFF = STATE_VALUE_TYPE_OFF + 3
const STATE_VERSION_OFF = STATE_KEY_OFF + STATE_KEY_SIZE
const STATE_VALUE_BOOL_OFF = STATE_VERSION_OFF + 4
const STATE_VALUE_F32_OFF = STATE_VALUE_BOOL_OFF + 4
const STATE_VALUE_U32_OFF = STATE_VALUE_F32_OFF + 4
const STATE_VALUE_U64_OFF = STATE_VALUE_U32_OFF + 4
const STATE_VALUE_TEXT_OFF = STATE_VALUE_U64_OFF + 8
const AUTO_ID_SIZE = 32
const AUTO_NAME_SIZE = 48
const AUTO_MAX_TRIGGERS = 8
const AUTO_MAX_CONDITIONS = 16
const AUTO_MAX_ACTIONS = 16
const AUTO_MAX_STRING_TABLE_BYTES = 512
const AUTO_TRIGGER_SIZE = 16
const AUTO_CONDITION_SIZE = 22
const AUTO_ACTION_SIZE = 32
const AUTO_TRIGGERS_OFF = 86
const AUTO_CONDITIONS_OFF = AUTO_TRIGGERS_OFF + AUTO_MAX_TRIGGERS * AUTO_TRIGGER_SIZE
const AUTO_ACTIONS_OFF = AUTO_CONDITIONS_OFF + AUTO_MAX_CONDITIONS * AUTO_CONDITION_SIZE
const AUTO_STRING_TABLE_SIZE_OFF = AUTO_ACTIONS_OFF + AUTO_MAX_ACTIONS * AUTO_ACTION_SIZE
const AUTO_STRING_TABLE_OFF = AUTO_STRING_TABLE_SIZE_OFF + 2

function readFixedString(view, offset, size) {
  const bytes = []
  for (let i = 0; i < size; i += 1) {
    const b = view.getUint8(offset + i)
    if (b === 0) break
    bytes.push(b)
  }
  return new TextDecoder().decode(new Uint8Array(bytes))
}

function normalizeUid(v) {
  return String(v ?? '').trim().toLowerCase()
}

function clusterList(view, offset, count, maxCount = 16) {
  const n = Math.max(0, Math.min(Number(count) || 0, maxCount))
  const out = []
  for (let i = 0; i < n; i += 1) {
    out.push(view.getUint16(offset + i * 2, true))
  }
  return out
}

function hasCluster(list, clusterId) {
  return (Array.isArray(list) ? list : []).includes(clusterId)
}

function deriveEndpointMeta(ep) {
  const inClusters = Array.isArray(ep?.in_clusters) ? ep.in_clusters : []
  const outClusters = Array.isArray(ep?.out_clusters) ? ep.out_clusters : []
  const accepts = []
  const emits = []
  const reports = []
  let kind = 'unknown'

  if (hasCluster(inClusters, 0x0006)) {
    accepts.push('onoff.on', 'onoff.off', 'onoff.toggle')
  }
  if (hasCluster(inClusters, 0x0008)) {
    accepts.push('level.move_to_level')
  }
  if (hasCluster(inClusters, 0x0300)) {
    accepts.push('color.move_to_color_xy', 'color.move_to_color_temperature')
  }
  if (hasCluster(inClusters, 0x0402)) {
    reports.push('temperature_c')
  }
  if (hasCluster(inClusters, 0x0405)) {
    reports.push('humidity_pct')
  }
  if (hasCluster(inClusters, 0x0001)) {
    reports.push('battery_pct')
  }
  if (hasCluster(inClusters, 0x0406)) {
    reports.push('occupancy')
  }
  if (hasCluster(outClusters, 0x0006)) {
    emits.push('onoff')
  }
  if (hasCluster(outClusters, 0x0008)) {
    emits.push('level')
  }

  if (hasCluster(inClusters, 0x0402) || hasCluster(inClusters, 0x0405)) {
    kind = 'temp_humidity_sensor'
  } else if (hasCluster(inClusters, 0x0006) && hasCluster(inClusters, 0x0008) && hasCluster(inClusters, 0x0300)) {
    kind = 'color_light'
  } else if (hasCluster(inClusters, 0x0006) && hasCluster(inClusters, 0x0008)) {
    kind = 'dimmable_light'
  } else if (hasCluster(inClusters, 0x0006)) {
    kind = 'relay'
  } else if (hasCluster(outClusters, 0x0006) && hasCluster(outClusters, 0x0008)) {
    kind = 'dimmer_switch'
  } else if (hasCluster(outClusters, 0x0006)) {
    kind = 'switch'
  }

  return { kind, accepts, emits, reports }
}

function ensureDevice(snapshot, uid) {
  const key = normalizeUid(uid)
  if (!key) return null
  if (!snapshot.devicesByUid[key]) {
    snapshot.devicesByUid[key] = {
      device_uid: uid,
      short_addr: 0,
      last_seen_ms: 0,
      has_onoff: false,
      has_button: false,
      name: '',
      endpoints: [],
      sensors: [],
      state: {},
    }
  }
  return snapshot.devicesByUid[key]
}

function ensureEndpoint(device, endpointId) {
  const epNum = Number(endpointId || 0)
  if (!device || epNum <= 0) return null
  let ep = (Array.isArray(device.endpoints) ? device.endpoints : []).find((x) => Number(x?.endpoint || 0) === epNum)
  if (!ep) {
    ep = {
      endpoint: epNum,
      profile_id: 0,
      device_id: 0,
      in_clusters: [],
      out_clusters: [],
      kind: 'unknown',
      accepts: [],
      emits: [],
      reports: [],
      label: '',
      live_state: {},
      sensors: [],
    }
    device.endpoints = [...device.endpoints, ep]
  }
  return ep
}

function sortSnapshotDevices(snapshot) {
  const devices = Object.values(snapshot.devicesByUid)
  devices.sort((a, b) => String(a?.device_uid ?? '').localeCompare(String(b?.device_uid ?? '')))
  devices.forEach((d) => {
    d.endpoints = [...(Array.isArray(d.endpoints) ? d.endpoints : [])].sort(
      (a, b) => Number(a?.endpoint ?? 0) - Number(b?.endpoint ?? 0),
    )
  })
  return devices
}

function parseValue(payload) {
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)
  const valueType = view.getUint8(STATE_VALUE_TYPE_OFF)
  switch (valueType) {
    case 1:
      return view.getUint8(STATE_VALUE_BOOL_OFF) !== 0
    case 2:
      return view.getFloat32(STATE_VALUE_F32_OFF, true)
    case 3:
      return view.getUint32(STATE_VALUE_U32_OFF, true)
    case 4:
      return Number(view.getBigUint64(STATE_VALUE_U64_OFF, true))
    case 5:
      return readFixedString(view, STATE_VALUE_TEXT_OFF, STATE_TEXT_SIZE)
    default:
      return null
  }
}

export function protoEncodeSnapshotRequest(seq = 1) {
  const buf = new ArrayBuffer(HDR_SIZE)
  const view = new DataView(buf)
  view.setUint8(0, GW_PROTO_VERSION_V1)
  view.setUint8(1, GW_PROTO_MSG_SNAPSHOT_REQUEST)
  view.setUint16(2, 0, true)
  view.setUint16(4, seq, true)
  view.setUint16(6, 0, true)
  return buf
}

export function protoTryParseFrame(bufferLike) {
  if (!(bufferLike instanceof ArrayBuffer)) return null
  if (bufferLike.byteLength < HDR_SIZE) return null
  const view = new DataView(bufferLike)
  const version = view.getUint8(0)
  const type = view.getUint8(1)
  const len = view.getUint16(2, true)
  const seq = view.getUint16(4, true)
  if (version !== GW_PROTO_VERSION_V1) return null
  if (bufferLike.byteLength !== HDR_SIZE + len) return null
  const payload = new Uint8Array(bufferLike, HDR_SIZE, len)
  return { version, type, len, seq, payload }
}

export function protoCreateSnapshotAccumulator() {
  return {
    devicesByUid: {},
    deviceStates: {},
    automationsById: {},
    inSync: false,
    currentScope: 0,
    seq: 0,
  }
}

function automationEventTypeToString(type) {
  switch (Number(type || 0)) {
    case 1: return 'zigbee.command'
    case 2: return 'zigbee.attr_report'
    case 3: return 'device.join'
    case 4: return 'device.leave'
    default: return 'zigbee.command'
  }
}

function automationOpToString(op) {
  switch (Number(op || 0)) {
    case 1: return '=='
    case 2: return '!='
    case 3: return '>'
    case 4: return '<'
    case 5: return '>='
    case 6: return '<='
    default: return '=='
  }
}

function automationReadStringTable(view, offset, size, tableStart, tableSize) {
  const off = Number(offset || 0)
  if (!off || off >= tableSize) return ''
  return readFixedString(view, tableStart + off, Math.max(0, tableSize - off))
}

function automationHex16(value) {
  const v = Number(value || 0) & 0xffff
  return `0x${v.toString(16).padStart(4, '0')}`
}

function automationDecodeTrigger(view, base, entry) {
  const eventType = view.getUint8(base)
  const endpoint = view.getUint8(base + 1)
  const deviceUidOff = view.getUint32(base + 4, true)
  const cmdOff = view.getUint32(base + 8, true)
  const clusterId = view.getUint16(base + 12, true)
  const attrId = view.getUint16(base + 14, true)

  const match = {}
  const deviceUid = automationReadStringTable(view, deviceUidOff, AUTO_MAX_STRING_TABLE_BYTES, entry.tableStart, entry.tableSize)
  const cmd = automationReadStringTable(view, cmdOff, AUTO_MAX_STRING_TABLE_BYTES, entry.tableStart, entry.tableSize)
  if (deviceUid) match.device_uid = deviceUid
  if (endpoint) match['payload.endpoint'] = endpoint
  if (eventType === 1) {
    if (cmd) match['payload.cmd'] = cmd
    if (clusterId) match['payload.cluster'] = automationHex16(clusterId)
  } else if (eventType === 2) {
    if (clusterId) match['payload.cluster'] = automationHex16(clusterId)
    if (attrId) match['payload.attr'] = automationHex16(attrId)
  }

  return {
    type: 'event',
    event_type: automationEventTypeToString(eventType),
    match,
  }
}

function automationDecodeCondition(view, base, entry) {
  const op = view.getUint8(base)
  const valType = view.getUint8(base + 1)
  const endpoint = view.getUint8(base + 2)
  const deviceUidOff = view.getUint32(base + 6, true)
  const keyOff = view.getUint32(base + 10, true)
  const deviceUid = automationReadStringTable(view, deviceUidOff, AUTO_MAX_STRING_TABLE_BYTES, entry.tableStart, entry.tableSize)
  const key = automationReadStringTable(view, keyOff, AUTO_MAX_STRING_TABLE_BYTES, entry.tableStart, entry.tableSize)
  const value = valType === 2 ? (view.getUint8(base + 14) !== 0) : view.getFloat64(base + 14, true)

  const ref = { device_uid: deviceUid, key }
  if (endpoint) ref.endpoint = endpoint

  return {
    type: 'state',
    op: automationOpToString(op),
    ref,
    value,
  }
}

function automationDecodeAction(view, base, entry) {
  const kind = view.getUint8(base)
  const endpoint = view.getUint8(base + 1)
  const auxEndpoint = view.getUint8(base + 2)
  const u16_0 = view.getUint16(base + 4, true)
  const u16_1 = view.getUint16(base + 6, true)
  const cmdOff = view.getUint32(base + 8, true)
  const uidOff = view.getUint32(base + 12, true)
  const uid2Off = view.getUint32(base + 16, true)
  const arg0 = view.getUint32(base + 20, true)
  const arg1 = view.getUint32(base + 24, true)
  const arg2 = view.getUint32(base + 28, true)

  const cmd = automationReadStringTable(view, cmdOff, AUTO_MAX_STRING_TABLE_BYTES, entry.tableStart, entry.tableSize)
  const uid = automationReadStringTable(view, uidOff, AUTO_MAX_STRING_TABLE_BYTES, entry.tableStart, entry.tableSize)
  const uid2 = automationReadStringTable(view, uid2Off, AUTO_MAX_STRING_TABLE_BYTES, entry.tableStart, entry.tableSize)

  const out = { type: 'zigbee', cmd }
  if (kind === 4) {
    out.src_device_uid = uid
    out.src_endpoint = endpoint
    out.cluster_id = automationHex16(u16_0)
    out.dst_device_uid = uid2
    out.dst_endpoint = auxEndpoint
    return out
  }
  if (kind === 3) {
    out.group_id = automationHex16(u16_0)
    out.scene_id = u16_1
    return out
  }
  if (kind === 2) {
    out.group_id = automationHex16(u16_0)
  } else if (kind === 1) {
    out.device_uid = uid
    out.endpoint = endpoint
  }

  if (cmd === 'level.move_to_level') {
    out.level = arg0
    out.transition_ms = arg1
  } else if (cmd === 'color.move_to_color_xy') {
    out.x = arg0
    out.y = arg1
    out.transition_ms = arg2
  } else if (cmd === 'color.move_to_color_temperature') {
    out.mireds = arg0
    out.transition_ms = arg1
  }
  return out
}

function decodeAutomationEntry(payload) {
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)
  const id = readFixedString(view, 0, AUTO_ID_SIZE)
  const name = readFixedString(view, AUTO_ID_SIZE, AUTO_NAME_SIZE)
  const enabled = view.getUint8(80) !== 0
  const triggersCount = Math.min(view.getUint8(82), AUTO_MAX_TRIGGERS)
  const conditionsCount = Math.min(view.getUint8(83), AUTO_MAX_CONDITIONS)
  const actionsCount = Math.min(view.getUint8(84), AUTO_MAX_ACTIONS)
  const tableSize = Math.min(view.getUint16(AUTO_STRING_TABLE_SIZE_OFF, true), AUTO_MAX_STRING_TABLE_BYTES)
  const entry = {
    tableStart: AUTO_STRING_TABLE_OFF,
    tableSize,
  }

  const triggers = []
  const conditions = []
  const actions = []

  for (let i = 0; i < triggersCount; i += 1) {
    triggers.push(automationDecodeTrigger(view, AUTO_TRIGGERS_OFF + i * AUTO_TRIGGER_SIZE, entry))
  }
  for (let i = 0; i < conditionsCount; i += 1) {
    conditions.push(automationDecodeCondition(view, AUTO_CONDITIONS_OFF + i * AUTO_CONDITION_SIZE, entry))
  }
  for (let i = 0; i < actionsCount; i += 1) {
    actions.push(automationDecodeAction(view, AUTO_ACTIONS_OFF + i * AUTO_ACTION_SIZE, entry))
  }

  const automation = {
    v: 1,
    id,
    name,
    enabled,
    mode: 'single',
    triggers,
    conditions,
    actions,
  }

  return {
    id,
    name,
    enabled,
    automation,
  }
}

function sortSnapshotAutomations(acc) {
	const items = Object.values(acc.automationsById)
	items.sort((a, b) => String(a?.id ?? '').localeCompare(String(b?.id ?? '')))
	return items
}

export function protoFrameToEvent(frame) {
  if (!frame?.payload) return null
  const payload = frame.payload
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)
  const ts_ms = Date.now()

  if (frame.type === GW_PROTO_MSG_SYNC_BEGIN) {
    return {
      ts_ms,
      type: 'gateway.event',
      data: {
        event_type: 'proto.sync_begin',
        source: 'gw_proto',
        msg: `seq=${frame.seq}`,
      },
    }
  }

  if (frame.type === GW_PROTO_MSG_SYNC_END) {
    return {
      ts_ms,
      type: 'gateway.event',
      data: {
        event_type: 'proto.sync_end',
        source: 'gw_proto',
        msg: `seq=${frame.seq}`,
      },
    }
  }

  if (frame.type === GW_PROTO_MSG_DEVICE_UPSERT) {
    const uid = readFixedString(view, 0, DEVICE_UID_SIZE)
    return {
      ts_ms,
      type: 'device.event',
      data: {
        device_id: uid,
        event: 'updated',
        source: 'gw_proto',
      },
    }
  }

  if (frame.type === GW_PROTO_MSG_DEVICE_REMOVE) {
    const uid = readFixedString(view, 0, DEVICE_UID_SIZE)
    return {
      ts_ms,
      type: 'device.event',
      data: {
        device_id: uid,
        event: 'leave',
        source: 'gw_proto',
      },
    }
  }

  if (frame.type === GW_PROTO_MSG_STATE_ITEM) {
    const uid = readFixedString(view, 0, DEVICE_UID_SIZE)
    const endpointId = Number(view.getUint8(STATE_ENDPOINT_OFF) || 0)
    const key = readFixedString(view, STATE_KEY_OFF, STATE_KEY_SIZE)
    return {
      ts_ms,
      type: 'device.state',
      data: {
        device_id: uid,
        endpoint_id: endpointId,
        key,
        value: parseValue(payload),
      },
    }
  }

  if (frame.type === GW_PROTO_MSG_SETTINGS) {
    return {
      ts_ms,
      type: 'gateway.event',
      data: {
        event_type: 'settings.changed',
        source: 'gw_proto',
        msg: 'settings updated',
      },
    }
  }

  if (frame.type === GW_PROTO_MSG_AUTOMATION_UPSERT) {
    const item = decodeAutomationEntry(payload)
    return {
      ts_ms,
      type: 'gateway.event',
      data: {
        event_type: 'automation.changed',
        source: 'gw_proto',
        msg: `automation ${item.id} updated`,
      },
    }
  }

  if (frame.type === GW_PROTO_MSG_AUTOMATION_REMOVE) {
    const id = readFixedString(view, 0, AUTO_ID_SIZE)
    return {
      ts_ms,
      type: 'gateway.event',
      data: {
        event_type: 'automation.changed',
        source: 'gw_proto',
        msg: `automation ${id} removed`,
      },
    }
  }

  return null
}

export function protoParseSettingsFrame(frame) {
  if (!frame?.payload || frame.type !== GW_PROTO_MSG_SETTINGS) return null
  const payload = frame.payload
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)
  return {
    screensaver_timeout_ms: Number(view.getUint32(0, true)),
    weather_success_interval_ms: Number(view.getUint32(4, true)),
    weather_retry_interval_ms: Number(view.getUint32(8, true)),
    timezone_auto: view.getUint8(12) !== 0,
    timezone_offset_min: view.getInt16(14, true),
  }
}

export function protoApplyFrame(acc, frame, onCommit) {
	if (!acc || !frame) return
	const payload = frame.payload
	const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)

	if (frame.type === GW_PROTO_MSG_SYNC_BEGIN) {
		acc.currentScope = view.getUint8(0)
		acc.inSync = true
		acc.seq = frame.seq
    if (acc.currentScope === GW_PROTO_SYNC_SCOPE_DEVICES) {
      acc.devicesByUid = {}
      acc.deviceStates = {}
    } else if (acc.currentScope === GW_PROTO_SYNC_SCOPE_AUTOMATIONS) {
      acc.automationsById = {}
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_SYNC_END) {
    const scope = view.getUint8(0)
    acc.inSync = false
    acc.currentScope = 0
    if (scope === GW_PROTO_SYNC_SCOPE_DEVICES && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    } else if (scope === GW_PROTO_SYNC_SCOPE_AUTOMATIONS && typeof onCommit === 'function') {
      onCommit({
        automations: sortSnapshotAutomations(acc),
      })
    }
    return
  }

	if (frame.type === GW_PROTO_MSG_DEVICE_UPSERT) {
		const uid = readFixedString(view, 0, DEVICE_UID_SIZE)
		const device = ensureDevice(acc, uid)
		if (!device) return
    device.device_uid = uid
    device.short_addr = view.getUint16(DEVICE_SHORT_ADDR_OFF, true)
		device.name = readFixedString(view, DEVICE_NAME_OFF, DEVICE_NAME_SIZE)
		device.last_seen_ms = Number(view.getBigUint64(DEVICE_LAST_SEEN_OFF, true))
		device.has_onoff = view.getUint8(DEVICE_HAS_ONOFF_OFF) !== 0
		device.has_button = view.getUint8(DEVICE_HAS_BUTTON_OFF) !== 0
		if ((!acc.inSync || acc.currentScope !== GW_PROTO_SYNC_SCOPE_DEVICES) && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_DEVICE_REMOVE) {
    const uid = normalizeUid(readFixedString(view, 0, DEVICE_UID_SIZE))
    if (!uid) return
    delete acc.devicesByUid[uid]
    delete acc.deviceStates[uid]
    if ((!acc.inSync || acc.currentScope !== GW_PROTO_SYNC_SCOPE_DEVICES) && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

	if (frame.type === GW_PROTO_MSG_ENDPOINT_UPSERT) {
		const uid = readFixedString(view, 0, DEVICE_UID_SIZE)
    const device = ensureDevice(acc, uid)
    if (!device) return
    device.short_addr = view.getUint16(ENDPOINT_SHORT_ADDR_OFF, true)
    const endpointId = view.getUint8(ENDPOINT_ID_OFF)
    const ep = ensureEndpoint(device, endpointId)
    if (!ep) return
    ep.profile_id = view.getUint16(ENDPOINT_PROFILE_ID_OFF, true)
    ep.device_id = view.getUint16(ENDPOINT_DEVICE_ID_OFF, true)
		ep.in_clusters = clusterList(view, ENDPOINT_IN_CLUSTERS_OFF, view.getUint8(ENDPOINT_IN_CLUSTER_COUNT_OFF), 16)
		ep.out_clusters = clusterList(view, ENDPOINT_OUT_CLUSTERS_OFF, view.getUint8(ENDPOINT_OUT_CLUSTER_COUNT_OFF), 16)
		const meta = deriveEndpointMeta(ep)
		ep.kind = meta.kind
		ep.accepts = meta.accepts
		ep.emits = meta.emits
		ep.reports = meta.reports
		if (!acc.inSync && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_ENDPOINT_REMOVE) {
    const uid = normalizeUid(readFixedString(view, 0, DEVICE_UID_SIZE))
    const endpointId = Number(view.getUint8(ENDPOINT_ID_OFF) || 0)
    const device = acc.devicesByUid[uid]
    if (device && endpointId > 0) {
      device.endpoints = (Array.isArray(device.endpoints) ? device.endpoints : []).filter((ep) => Number(ep?.endpoint ?? 0) !== endpointId)
      if (acc.deviceStates[uid]) {
        delete acc.deviceStates[uid][String(endpointId)]
      }
      if ((!acc.inSync || acc.currentScope !== GW_PROTO_SYNC_SCOPE_DEVICES) && typeof onCommit === 'function') {
        onCommit({
          devices: sortSnapshotDevices(acc),
          deviceStates: { ...acc.deviceStates },
        })
      }
    }
    return
  }

	if (frame.type === GW_PROTO_MSG_STATE_ITEM) {
		const uid = normalizeUid(readFixedString(view, 0, DEVICE_UID_SIZE))
    if (!uid) return
    const endpointId = String(view.getUint8(STATE_ENDPOINT_OFF))
    const key = readFixedString(view, STATE_KEY_OFF, STATE_KEY_SIZE)
    if (!endpointId || !key) return
    if (!acc.deviceStates[uid]) acc.deviceStates[uid] = {}
    if (!acc.deviceStates[uid][endpointId]) acc.deviceStates[uid][endpointId] = {}
		acc.deviceStates[uid][endpointId][key] = parseValue(payload)

		const device = ensureDevice(acc, uid)
    const ep = ensureEndpoint(device, Number(endpointId))
    if (ep) {
      ep.live_state = acc.deviceStates[uid][endpointId]
    }

    if ((!acc.inSync || acc.currentScope !== GW_PROTO_SYNC_SCOPE_DEVICES) && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_STATE_REMOVE) {
    const uid = normalizeUid(readFixedString(view, 0, DEVICE_UID_SIZE))
    const endpointId = String(view.getUint8(STATE_ENDPOINT_OFF))
    const key = readFixedString(view, STATE_REMOVE_KEY_OFF, STATE_KEY_SIZE)
    if (uid && endpointId && key && acc.deviceStates[uid]?.[endpointId]) {
      delete acc.deviceStates[uid][endpointId][key]
      if ((!acc.inSync || acc.currentScope !== GW_PROTO_SYNC_SCOPE_DEVICES) && typeof onCommit === 'function') {
        onCommit({
          devices: sortSnapshotDevices(acc),
          deviceStates: { ...acc.deviceStates },
        })
      }
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_AUTOMATION_UPSERT) {
    const item = decodeAutomationEntry(payload)
    if (item?.id) {
      acc.automationsById[item.id] = item
      if ((!acc.inSync || acc.currentScope !== GW_PROTO_SYNC_SCOPE_AUTOMATIONS) && typeof onCommit === 'function') {
        onCommit({
          automations: sortSnapshotAutomations(acc),
        })
      }
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_AUTOMATION_REMOVE) {
    const id = readFixedString(view, 0, AUTO_ID_SIZE)
    if (id && acc.automationsById[id]) {
      delete acc.automationsById[id]
      if ((!acc.inSync || acc.currentScope !== GW_PROTO_SYNC_SCOPE_AUTOMATIONS) && typeof onCommit === 'function') {
        onCommit({
          automations: sortSnapshotAutomations(acc),
        })
      }
    }
  }
}
