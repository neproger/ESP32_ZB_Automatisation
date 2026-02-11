export function defaultAutomationDef({ id, name, enabled }) {
	return {
		v: 1,
		id: String(id ?? ''),
		name: String(name ?? ''),
		enabled: Boolean(enabled),
		triggers: [
			{
				type: 'event',
				event_type: 'zigbee.command',
				match: { 'payload.cmd': 'toggle' },
			},
		],
		conditions: [],
		actions: [],
		mode: 'single',
	}
}

export function ensureArray(v) {
	return Array.isArray(v) ? v : []
}

const UID_RE = /^0x[0-9a-fA-F]{16}$/

export function normalizeUid(v) {
	return String(v ?? '').trim()
}

export function isValidUid(v) {
	const s = normalizeUid(v)
	return UID_RE.test(s)
}

export function normalizeEndpoint(v) {
	const n = Number(v)
	if (!Number.isFinite(n)) return NaN
	return Math.trunc(n)
}

export function isValidEndpoint(v) {
	const n = normalizeEndpoint(v)
	return n >= 1 && n <= 240
}

export function getDeviceOptions(devices) {
	const list = []
	for (const d of Array.isArray(devices) ? devices : []) {
		const uid = String(d?.device_uid ?? '')
		if (!uid) continue
		const name = String(d?.name ?? '')
		list.push({ uid, name, label: name ? `${name} (${uid})` : uid })
	}
	return list
}

export function getEndpointsForUid(endpointsByUid, uid) {
	const u = String(uid ?? '')
	const eps = u ? endpointsByUid?.[u] : null
	return Array.isArray(eps) ? eps : []
}

export function getEndpointOptions(endpointsByUid, uid) {
	const map = new Map()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		const n = Number(ep?.endpoint ?? 0)
		if (!n || map.has(n)) continue
		map.set(n, String(ep?.kind ?? '').trim())
	}
	const uniq = Array.from(map.keys()).sort((a, b) => a - b)
	const list = uniq.length ? uniq : [1]
	return list.map((ep) => ({ endpoint: ep, kind: map.get(ep) }))
}

export function getEmitsByPrefix(endpointsByUid, uid, prefix) {
	const set = new Set()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		for (const s of Array.isArray(ep?.emits) ? ep.emits : []) {
			const v = String(s ?? '')
			if (!v) continue
			if (prefix) {
				if (v.startsWith(prefix)) set.add(v.slice(prefix.length))
			} else {
				set.add(v)
			}
		}
	}
	return Array.from(set).sort()
}

export function getReports(endpointsByUid, uid) {
	const set = new Set()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		for (const s of Array.isArray(ep?.reports) ? ep.reports : []) {
			const v = String(s ?? '')
			if (v) set.add(v)
		}
	}
	return Array.from(set).sort()
}

export function getAcceptsByPrefix(endpointsByUid, uid, endpoint, prefixes) {
	const set = new Set()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		if (Number(ep?.endpoint ?? 0) !== Number(endpoint ?? 0)) continue
		for (const s of Array.isArray(ep?.accepts) ? ep.accepts : []) {
			const v = String(s ?? '')
			if (!v) continue
			if (!prefixes || prefixes.length === 0) {
				set.add(v)
				continue
			}
			for (const p of prefixes) {
				if (v.startsWith(p)) {
					set.add(v)
					break
				}
			}
		}
	}
	return Array.from(set).sort()
}

export function toNumberOrString(v) {
	const s = String(v ?? '').trim()
	if (!s) return ''
	if (/^-?\d+(\.\d+)?$/.test(s)) return Number(s)
	return s
}

const CONDITION_KEY_INFO = {
	onoff: { label: 'On/Off', valueType: 'bool' },
	occupancy: { label: 'Occupancy', valueType: 'bool' },
	temperature_c: { label: 'Temperature (C)', valueType: 'number' },
	humidity_pct: { label: 'Humidity (%)', valueType: 'number' },
	battery_pct: { label: 'Battery (%)', valueType: 'number' },
	level: { label: 'Level', valueType: 'number' },
	illuminance: { label: 'Illuminance', valueType: 'number' },
}

export function conditionKeyType(key) {
	const k = String(key ?? '')
	return CONDITION_KEY_INFO[k]?.valueType ?? 'number'
}

export function conditionKeyLabel(key) {
	const k = String(key ?? '')
	const meta = CONDITION_KEY_INFO[k]
	return meta ? `${meta.label} (${k})` : k
}

export function getReportOptions(endpointsByUid, uid, endpoint) {
	const epFilter = endpoint === '' || endpoint === undefined || endpoint === null ? null : Number(endpoint)
	const map = new Map()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		const epNum = Number(ep?.endpoint ?? 0)
		if (epFilter != null && epNum !== epFilter) continue
		for (const raw of Array.isArray(ep?.reports) ? ep.reports : []) {
			const key = String(raw ?? '')
			if (!key) continue
			if (!map.has(key)) {
				map.set(key, { key, endpoints: new Set([epNum]) })
			} else {
				map.get(key).endpoints.add(epNum)
			}
		}
	}
	return Array.from(map.values())
		.map((x) => ({
			key: x.key,
			label: conditionKeyLabel(x.key),
			valueType: conditionKeyType(x.key),
			endpoints: Array.from(x.endpoints).sort((a, b) => a - b),
		}))
		.sort((a, b) => a.key.localeCompare(b.key))
}
