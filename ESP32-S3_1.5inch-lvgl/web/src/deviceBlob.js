const DFB1_MAGIC = 0x31424644
const BLOB_VERSION = 1

const UID_LEN = 19
const NAME_LEN = 32
const MAX_CLUSTERS = 8

const HEADER_SIZE = 12
const DEVICE_SIZE = UID_LEN + 2 + 8 + 1 + 1 + NAME_LEN
const ENDPOINT_SIZE = UID_LEN + 2 + 1 + 2 + 2 + 1 + 1 + (MAX_CLUSTERS * 2) + (MAX_CLUSTERS * 2)

const ZCL = {
	BASIC: 0x0000,
	POWER_CONFIG: 0x0001,
	GROUPS: 0x0004,
	SCENES: 0x0005,
	ONOFF: 0x0006,
	LEVEL: 0x0008,
	COLOR_CONTROL: 0x0300,
	ILLUMINANCE: 0x0400,
	TEMPERATURE: 0x0402,
	PRESSURE: 0x0403,
	FLOW: 0x0404,
	HUMIDITY: 0x0405,
	OCCUPANCY: 0x0406,
}

function readCString(bytes, offset, len) {
	let end = offset
	const max = offset + len
	while (end < max && bytes[end] !== 0) end += 1
	return new TextDecoder().decode(bytes.subarray(offset, end)).trim()
}

function readU64(view, offset) {
	if (typeof view.getBigUint64 === 'function') {
		return Number(view.getBigUint64(offset, true))
	}
	const lo = view.getUint32(offset, true)
	const hi = view.getUint32(offset + 4, true)
	return hi * 0x100000000 + lo
}

function hasCluster(list, clusterId) {
	return Array.isArray(list) && list.includes(clusterId)
}

function classifyKind(ep) {
	const onoffSrv = hasCluster(ep.in_clusters, ZCL.ONOFF)
	const onoffCli = hasCluster(ep.out_clusters, ZCL.ONOFF)
	const levelSrv = hasCluster(ep.in_clusters, ZCL.LEVEL)
	const colorSrv = hasCluster(ep.in_clusters, ZCL.COLOR_CONTROL)

	const tempSrv = hasCluster(ep.in_clusters, ZCL.TEMPERATURE)
	const humSrv = hasCluster(ep.in_clusters, ZCL.HUMIDITY)
	const occSrv = hasCluster(ep.in_clusters, ZCL.OCCUPANCY)
	const illumSrv = hasCluster(ep.in_clusters, ZCL.ILLUMINANCE)
	const pressSrv = hasCluster(ep.in_clusters, ZCL.PRESSURE)
	const flowSrv = hasCluster(ep.in_clusters, ZCL.FLOW)

	if (colorSrv) return 'color_light'
	if (levelSrv && onoffSrv) return 'dimmable_light'
	if (onoffSrv) return 'relay'

	if (onoffCli) {
		if (hasCluster(ep.out_clusters, ZCL.LEVEL)) return 'dimmer_switch'
		return 'switch'
	}

	const anySensor = tempSrv || humSrv || occSrv || illumSrv || pressSrv || flowSrv
	if (anySensor) {
		if (tempSrv && humSrv) return 'temp_humidity_sensor'
		if (tempSrv) return 'temperature_sensor'
		if (humSrv) return 'humidity_sensor'
		if (occSrv) return 'occupancy_sensor'
		if (illumSrv) return 'illuminance_sensor'
		if (pressSrv) return 'pressure_sensor'
		if (flowSrv) return 'flow_sensor'
		return 'sensor'
	}
	return 'unknown'
}

function endpointAccepts(ep) {
	const out = []
	if (hasCluster(ep.in_clusters, ZCL.ONOFF)) {
		out.push('onoff.off', 'onoff.on', 'onoff.toggle', 'onoff.off_with_effect', 'onoff.on_with_recall_global_scene', 'onoff.on_with_timed_off')
	}
	if (hasCluster(ep.in_clusters, ZCL.LEVEL)) {
		out.push('level.move_to_level', 'level.move', 'level.step', 'level.stop', 'level.move_to_level_with_onoff', 'level.move_with_onoff', 'level.step_with_onoff', 'level.stop_with_onoff')
	}
	if (hasCluster(ep.in_clusters, ZCL.COLOR_CONTROL)) {
		out.push('color.move_to_hue', 'color.move_hue', 'color.step_hue', 'color.move_to_saturation', 'color.move_saturation', 'color.step_saturation', 'color.move_to_hue_saturation', 'color.move_to_color_xy', 'color.move_to_color_temperature', 'color.stop_move_step')
	}
	if (hasCluster(ep.in_clusters, ZCL.GROUPS)) out.push('groups.add', 'groups.remove')
	if (hasCluster(ep.in_clusters, ZCL.SCENES)) out.push('scenes.recall')
	return out
}

function endpointEmits(ep) {
	const out = []
	if (hasCluster(ep.out_clusters, ZCL.ONOFF)) out.push('onoff.off', 'onoff.on', 'onoff.toggle')
	if (hasCluster(ep.out_clusters, ZCL.LEVEL)) {
		out.push('level.move_to_level', 'level.move', 'level.step', 'level.stop', 'level.move_to_level_with_onoff', 'level.move_with_onoff', 'level.step_with_onoff', 'level.stop_with_onoff')
	}
	if (hasCluster(ep.out_clusters, ZCL.COLOR_CONTROL)) out.push('color.*')
	return out
}

function endpointReports(ep) {
	const out = []
	if (hasCluster(ep.in_clusters, ZCL.ONOFF)) out.push('onoff')
	if (hasCluster(ep.in_clusters, ZCL.LEVEL)) out.push('level')
	if (hasCluster(ep.in_clusters, ZCL.TEMPERATURE)) out.push('temperature_c')
	if (hasCluster(ep.in_clusters, ZCL.HUMIDITY)) out.push('humidity_pct')
	if (hasCluster(ep.in_clusters, ZCL.OCCUPANCY)) out.push('occupancy')
	if (hasCluster(ep.in_clusters, ZCL.ILLUMINANCE)) out.push('illuminance')
	if (hasCluster(ep.in_clusters, ZCL.POWER_CONFIG)) out.push('battery_pct')
	return out
}

export function parseDeviceBlob(buffer) {
	if (!(buffer instanceof ArrayBuffer)) throw new Error('device blob: invalid buffer')
	if (buffer.byteLength < HEADER_SIZE) throw new Error('device blob: too short')

	const view = new DataView(buffer)
	const bytes = new Uint8Array(buffer)

	const magic = view.getUint32(0, true)
	const version = view.getUint16(4, true)
	const deviceCount = view.getUint16(6, true)
	const endpointCount = view.getUint16(8, true)

	if (magic !== DFB1_MAGIC) throw new Error('device blob: bad magic')
	if (version !== BLOB_VERSION) throw new Error(`device blob: unsupported version ${version}`)

	const expectedSize = HEADER_SIZE + (deviceCount * DEVICE_SIZE) + (endpointCount * ENDPOINT_SIZE)
	if (buffer.byteLength < expectedSize) {
		throw new Error(`device blob: short payload ${buffer.byteLength} < ${expectedSize}`)
	}

	let off = HEADER_SIZE
	const devicesByUid = new Map()
	for (let i = 0; i < deviceCount; i += 1) {
		const uid = readCString(bytes, off, UID_LEN)
		off += UID_LEN
		const short_addr = view.getUint16(off, true)
		off += 2
		const last_seen_ms = readU64(view, off)
		off += 8
		const has_onoff = view.getUint8(off) !== 0
		off += 1
		const has_button = view.getUint8(off) !== 0
		off += 1
		const name = readCString(bytes, off, NAME_LEN)
		off += NAME_LEN

		if (!uid) continue
		devicesByUid.set(uid, {
			device_uid: uid,
			short_addr,
			last_seen_ms,
			has_onoff,
			has_button,
			name,
			endpoints: [],
			sensors: [],
			state: {},
		})
	}

	for (let i = 0; i < endpointCount; i += 1) {
		const uid = readCString(bytes, off, UID_LEN)
		off += UID_LEN
		const short_addr = view.getUint16(off, true)
		off += 2
		const endpoint = view.getUint8(off)
		off += 1
		const profile_id = view.getUint16(off, true)
		off += 2
		const device_id = view.getUint16(off, true)
		off += 2
		const in_cluster_count = Math.min(view.getUint8(off), MAX_CLUSTERS)
		off += 1
		const out_cluster_count = Math.min(view.getUint8(off), MAX_CLUSTERS)
		off += 1

		const allIn = new Array(MAX_CLUSTERS)
		for (let c = 0; c < MAX_CLUSTERS; c += 1) {
			allIn[c] = view.getUint16(off, true)
			off += 2
		}
		const allOut = new Array(MAX_CLUSTERS)
		for (let c = 0; c < MAX_CLUSTERS; c += 1) {
			allOut[c] = view.getUint16(off, true)
			off += 2
		}

		if (!uid) continue
		let d = devicesByUid.get(uid)
		if (!d) {
			d = {
				device_uid: uid,
				short_addr,
				last_seen_ms: 0,
				has_onoff: false,
				has_button: false,
				name: '',
				endpoints: [],
				sensors: [],
				state: {},
			}
			devicesByUid.set(uid, d)
		}

		const ep = {
			endpoint,
			profile_id,
			device_id,
			in_clusters: allIn.slice(0, in_cluster_count),
			out_clusters: allOut.slice(0, out_cluster_count),
		}
		ep.kind = classifyKind(ep)
		ep.accepts = endpointAccepts(ep)
		ep.emits = endpointEmits(ep)
		ep.reports = endpointReports(ep)
		d.endpoints.push(ep)
	}

	return Array.from(devicesByUid.values())
}
