//UTF-8
//groupsStore.js
import { fetchCbor, postCbor } from './api.js'

const CHANGED_EVENT = 'gw_groups_changed'

let sGroups = []
let sItems = []
let sMembers = {}
let sLabels = {}
let sReloadPromise = null

function notifyChanged() {
	window.dispatchEvent(new Event(CHANGED_EVENT))
}

function normalizeUid(v) {
	return String(v ?? '').trim().toLowerCase()
}

function endpointKey(deviceUid, endpoint) {
	return `${normalizeUid(deviceUid)}::${Number(endpoint ?? 0)}`
}

function normalizeGroupItems(items) {
	const list = Array.isArray(items) ? items : []
	return list
		.map((it) => {
			const group_id = String(it?.group_id ?? '')
			const device_uid = normalizeUid(it?.device_uid)
			const endpoint_id = Number(it?.endpoint_id ?? 0)
			const order = Number(it?.order ?? 0)
			const label = String(it?.label ?? '')
			return { group_id, device_uid, endpoint_id, order, label }
		})
		.filter((it) => it.group_id && it.device_uid && it.endpoint_id > 0)
}

function rebuildMaps(items) {
	const members = {}
	const labels = {}
	const list = normalizeGroupItems(items)
	list.forEach((it) => {
		const gid = String(it?.group_id ?? '')
		const uid = normalizeUid(it?.device_uid)
		const ep = Number(it?.endpoint_id ?? 0)
		if (!uid || ep <= 0) return
		const key = endpointKey(uid, ep)
		if (gid) members[key] = gid
		const label = String(it?.label ?? '').trim()
		if (label) labels[key] = label
	})
	sMembers = members
	sLabels = labels
	sItems = list
}

export function groupsSubscribe(onChange) {
	if (typeof onChange !== 'function') return () => {}
	const h = () => onChange()
	window.addEventListener(CHANGED_EVENT, h)
	return () => window.removeEventListener(CHANGED_EVENT, h)
}

export function groupsList() {
	return Array.isArray(sGroups) ? sGroups : []
}

export function groupItemsList() {
	return Array.isArray(sItems) ? sItems : []
}

export function groupMembersMap() {
	return sMembers && typeof sMembers === 'object' ? sMembers : {}
}

export function groupGetForEndpoint(deviceUid, endpoint) {
	return String(groupMembersMap()[endpointKey(deviceUid, endpoint)] ?? '')
}

export function endpointLabelGet(deviceUid, endpoint) {
	return String((sLabels && sLabels[endpointKey(deviceUid, endpoint)]) ?? '')
}

export async function groupsReload() {
	if (sReloadPromise) return sReloadPromise
	sReloadPromise = (async () => {
		const [groupsRes, itemsRes] = await Promise.all([
			fetchCbor('/api/groups'),
			fetchCbor('/api/groups/items'),
		])
		sGroups = Array.isArray(groupsRes?.groups) ? groupsRes.groups : []
		rebuildMaps(itemsRes?.items)
		notifyChanged()
	})()
	try {
		await sReloadPromise
	} finally {
		sReloadPromise = null
	}
}

export async function groupsCreate(name) {
	const n = String(name ?? '').trim()
	if (!n) return null
	const res = await postCbor('/api/groups', { op: 'create', name: n })
	await groupsReload()
	return String(res?.id ?? '')
}

export async function groupsRename(id, name) {
	const gid = String(id ?? '')
	const n = String(name ?? '').trim()
	if (!gid || !n) return false
	await postCbor('/api/groups', { op: 'rename', id: gid, name: n })
	await groupsReload()
	return true
}

export async function groupsDelete(id) {
	const gid = String(id ?? '')
	if (!gid) return false
	await postCbor('/api/groups', { op: 'delete', id: gid })
	await groupsReload()
	return true
}

export async function groupSetForEndpoint(deviceUid, endpoint, groupId) {
	const uid = normalizeUid(deviceUid)
	const ep = Number(endpoint ?? 0)
	const gid = String(groupId ?? '')
	if (!uid || ep <= 0) return false
	if (!gid) {
		await postCbor('/api/groups/items', { op: 'remove', device_uid: uid, endpoint_id: ep })
	} else {
		await postCbor('/api/groups/items', { op: 'set', group_id: gid, device_uid: uid, endpoint_id: ep })
	}
	await groupsReload()
	return true
}

export async function endpointLabelSet(deviceUid, endpoint, label) {
	const uid = normalizeUid(deviceUid)
	const ep = Number(endpoint ?? 0)
	if (!uid || ep <= 0) return false
	await postCbor('/api/groups/items', {
		op: 'label',
		device_uid: uid,
		endpoint_id: ep,
		label: String(label ?? ''),
	})
	await groupsReload()
	return true
}

export async function groupReorder(groupId, orderedItems) {
	const gid = String(groupId ?? '')
	if (!gid) return false
	const list = Array.isArray(orderedItems) ? orderedItems : []
	for (let i = 0; i < list.length; i += 1) {
		const it = list[i] || {}
		const uid = normalizeUid(it?.device_uid)
		const ep = Number(it?.endpoint_id ?? 0)
		if (!uid || ep <= 0) continue
		await postCbor('/api/groups/items', {
			op: 'reorder',
			group_id: gid,
			device_uid: uid,
			endpoint_id: ep,
			order: i + 1,
		})
	}
	await groupsReload()
	return true
}

export { endpointKey }
