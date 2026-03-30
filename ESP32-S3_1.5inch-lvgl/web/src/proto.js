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

const HDR_SIZE = 8

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
  const valueType = view.getUint8(19)
  switch (valueType) {
    case 1:
      return view.getUint8(52) !== 0
    case 2:
      return view.getFloat32(56, true)
    case 3:
      return view.getUint32(60, true)
    case 4:
      return Number(view.getBigUint64(64, true))
    case 5:
      return readFixedString(view, 72, 64)
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
    inSync: false,
    seq: 0,
  }
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
    const uid = readFixedString(view, 0, 19)
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
    const uid = readFixedString(view, 0, 19)
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
    const uid = readFixedString(view, 0, 19)
    const endpointId = Number(view.getUint8(19) || 0)
    const key = readFixedString(view, 22, 24)
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
    acc.devicesByUid = {}
    acc.deviceStates = {}
    acc.inSync = true
    acc.seq = frame.seq
    return
  }

  if (frame.type === GW_PROTO_MSG_SYNC_END) {
    acc.inSync = false
    if (typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_DEVICE_UPSERT) {
    const uid = readFixedString(view, 0, 19)
    const device = ensureDevice(acc, uid)
    if (!device) return
    device.device_uid = uid
    device.short_addr = view.getUint16(20, true)
    device.name = readFixedString(view, 22, 32)
    device.last_seen_ms = Number(view.getBigUint64(56, true))
    device.has_onoff = view.getUint8(64) !== 0
    device.has_button = view.getUint8(65) !== 0
    if (!acc.inSync && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_DEVICE_REMOVE) {
    const uid = normalizeUid(readFixedString(view, 0, 19))
    if (!uid) return
    delete acc.devicesByUid[uid]
    delete acc.deviceStates[uid]
    if (!acc.inSync && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_ENDPOINT_UPSERT) {
    const uid = readFixedString(view, 0, 19)
    const device = ensureDevice(acc, uid)
    if (!device) return
    device.short_addr = view.getUint16(20, true)
    const endpointId = view.getUint8(22)
    const ep = ensureEndpoint(device, endpointId)
    if (!ep) return
    ep.profile_id = view.getUint16(28, true)
    ep.device_id = view.getUint16(30, true)
    ep.in_clusters = clusterList(view, 34, view.getUint8(32), 16)
    ep.out_clusters = clusterList(view, 66, view.getUint8(33), 16)
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
    const uid = normalizeUid(readFixedString(view, 0, 19))
    const endpointId = Number(view.getUint8(19) || 0)
    const device = acc.devicesByUid[uid]
    if (device && endpointId > 0) {
      device.endpoints = (Array.isArray(device.endpoints) ? device.endpoints : []).filter((ep) => Number(ep?.endpoint ?? 0) !== endpointId)
      if (acc.deviceStates[uid]) {
        delete acc.deviceStates[uid][String(endpointId)]
      }
      if (!acc.inSync && typeof onCommit === 'function') {
        onCommit({
          devices: sortSnapshotDevices(acc),
          deviceStates: { ...acc.deviceStates },
        })
      }
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_STATE_ITEM) {
    const uid = normalizeUid(readFixedString(view, 0, 19))
    if (!uid) return
    const endpointId = String(view.getUint8(19))
    const key = readFixedString(view, 22, 24)
    if (!endpointId || !key) return
    if (!acc.deviceStates[uid]) acc.deviceStates[uid] = {}
    if (!acc.deviceStates[uid][endpointId]) acc.deviceStates[uid][endpointId] = {}
    acc.deviceStates[uid][endpointId][key] = parseValue(payload)

    const device = ensureDevice(acc, uid)
    const ep = ensureEndpoint(device, Number(endpointId))
    if (ep) {
      ep.live_state = acc.deviceStates[uid][endpointId]
    }

    if (!acc.inSync && typeof onCommit === 'function') {
      onCommit({
        devices: sortSnapshotDevices(acc),
        deviceStates: { ...acc.deviceStates },
      })
    }
    return
  }

  if (frame.type === GW_PROTO_MSG_STATE_REMOVE) {
    const uid = normalizeUid(readFixedString(view, 0, 19))
    const endpointId = String(view.getUint8(19))
    const key = readFixedString(view, 20, 24)
    if (uid && endpointId && key && acc.deviceStates[uid]?.[endpointId]) {
      delete acc.deviceStates[uid][endpointId][key]
      if (!acc.inSync && typeof onCommit === 'function') {
        onCommit({
          devices: sortSnapshotDevices(acc),
          deviceStates: { ...acc.deviceStates },
        })
      }
    }
  }
}
