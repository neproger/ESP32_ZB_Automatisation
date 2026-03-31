//UTF-8
//groupsStore.js
import {
	protoEncodeGroupCreate,
	protoEncodeGroupDelete,
	protoEncodeGroupItemLabel,
	protoEncodeGroupItemRemove,
	protoEncodeGroupItemReorder,
	protoEncodeGroupItemSet,
	protoEncodeGroupRename,
} from './proto.js'
import { sendWsCommand } from './wsCommandBus.js'

const CHANGED_EVENT = 'gw_groups_changed'
const GW_PROTO_MSG_SYNC_BEGIN = 0x40
const GW_PROTO_MSG_SYNC_END = 0x41
const GW_PROTO_MSG_GROUP_UPSERT = 0x48
const GW_PROTO_MSG_GROUP_REMOVE = 0x49
const GW_PROTO_MSG_GROUP_ITEM_UPSERT = 0x4a
const GW_PROTO_MSG_GROUP_ITEM_REMOVE = 0x4b

let sGroups = []
let sItems = []
let sMembers = {}
let sLabels = {}
let sPendingGroups = null
let sPendingItems = null
let sInProtoSync = false

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

function readFixedString(view, offset, size) {
	const bytes = []
	for (let i = 0; i < size; i += 1) {
		const b = view.getUint8(offset + i)
		if (b === 0) break
		bytes.push(b)
	}
	return new TextDecoder().decode(new Uint8Array(bytes)).replace(/[\u0000-\u001f\u007f]/g, '').trim()
}

function commitGroups(groups, items) {
	sGroups = Array.isArray(groups) ? groups : []
	rebuildMaps(items)
	notifyChanged()
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

export async function groupsCreate(name) {
	const n = String(name ?? '').trim()
	if (!n) return ''
	sendWsCommand(protoEncodeGroupCreate('', n, Date.now() & 0xffff))
	return 'queued'
}

export async function groupsRename(id, name) {
	const gid = String(id ?? '')
	const n = String(name ?? '').trim()
	if (!gid || !n) return false
	sendWsCommand(protoEncodeGroupRename(gid, n, Date.now() & 0xffff))
	return true
}

export async function groupsDelete(id) {
	const gid = String(id ?? '')
	if (!gid) return false
	sendWsCommand(protoEncodeGroupDelete(gid, Date.now() & 0xffff))
	return true
}

export async function groupSetForEndpoint(deviceUid, endpoint, groupId) {
	const uid = normalizeUid(deviceUid)
	const ep = Number(endpoint ?? 0)
	const gid = String(groupId ?? '')
	if (!uid || ep <= 0) return false
	if (!gid) {
		sendWsCommand(protoEncodeGroupItemRemove(uid, ep, Date.now() & 0xffff))
	} else {
		sendWsCommand(protoEncodeGroupItemSet(gid, uid, ep, Date.now() & 0xffff))
	}
	return true
}

export async function endpointLabelSet(deviceUid, endpoint, label) {
	const uid = normalizeUid(deviceUid)
	const ep = Number(endpoint ?? 0)
	if (!uid || ep <= 0) return false
	sendWsCommand(protoEncodeGroupItemLabel(uid, ep, String(label ?? ''), Date.now() & 0xffff))
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
		sendWsCommand(protoEncodeGroupItemReorder(gid, uid, ep, i + 1, Date.now() & 0xffff))
	}
	return true
}

export function groupsProtoApplyFrame(frame) {
	if (!frame?.payload) return false
	const payload = frame.payload
	const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength)

	if (frame.type === GW_PROTO_MSG_SYNC_BEGIN) {
		const scope = view.getUint8(0)
		if (scope === 3) {
			sPendingGroups = []
			sPendingItems = []
			sInProtoSync = true
			return true
		}
		return false
	}

	if (frame.type === GW_PROTO_MSG_SYNC_END) {
		const scope = view.getUint8(0)
		if (scope === 3) {
			sInProtoSync = false
			commitGroups(sPendingGroups || [], sPendingItems || [])
			sPendingGroups = null
			sPendingItems = null
			return true
		}
		return false
	}

	if (frame.type === GW_PROTO_MSG_GROUP_UPSERT) {
		const target = sInProtoSync && Array.isArray(sPendingGroups) ? sPendingGroups : [...sGroups]
		const group = {
			id: readFixedString(view, 0, 32),
			name: readFixedString(view, 32, 48),
			version: view.getUint32(80, true),
			created_at_ms: Number(view.getUint32(84, true)),
			updated_at_ms: Number(view.getUint32(88, true)),
		}
		const idx = target.findIndex((it) => String(it?.id ?? '') === group.id)
		if (idx >= 0) target[idx] = group
		else target.push(group)
		if (sInProtoSync) {
			sPendingGroups = target
		} else {
			commitGroups(target, sItems)
		}
		return true
	}

	if (frame.type === GW_PROTO_MSG_GROUP_REMOVE) {
		const groupId = readFixedString(view, 0, 32)
		const nextGroups = sGroups.filter((it) => String(it?.id ?? '') !== groupId)
		const nextItems = sItems.filter((it) => String(it?.group_id ?? '') !== groupId)
		if (sInProtoSync) {
			sPendingGroups = (sPendingGroups || []).filter((it) => String(it?.id ?? '') !== groupId)
			sPendingItems = (sPendingItems || []).filter((it) => String(it?.group_id ?? '') !== groupId)
		} else {
			commitGroups(nextGroups, nextItems)
		}
		return true
	}

	if (frame.type === GW_PROTO_MSG_GROUP_ITEM_UPSERT) {
		const target = sInProtoSync && Array.isArray(sPendingItems) ? sPendingItems : [...sItems]
		const item = {
			group_id: readFixedString(view, 0, 32),
			device_uid: normalizeUid(readFixedString(view, 32, 19)),
			endpoint_id: Number(view.getUint8(51) || 0),
			order: Number(view.getUint32(59, true)),
			label: readFixedString(view, 63, 32),
		}
		const idx = target.findIndex((it) => normalizeUid(it?.device_uid) === item.device_uid && Number(it?.endpoint_id ?? 0) === item.endpoint_id)
		if (idx >= 0) target[idx] = item
		else target.push(item)
		if (sInProtoSync) {
			sPendingItems = target
		} else {
			commitGroups(sGroups, target)
		}
		return true
	}

	if (frame.type === GW_PROTO_MSG_GROUP_ITEM_REMOVE) {
		const device_uid = normalizeUid(readFixedString(view, 0, 19))
		const endpoint_id = Number(view.getUint8(19) || 0)
		const match = (it) => !(normalizeUid(it?.device_uid) === device_uid && Number(it?.endpoint_id ?? 0) === endpoint_id)
		if (sInProtoSync) {
			sPendingItems = (sPendingItems || []).filter(match)
		} else {
			commitGroups(sGroups, sItems.filter(match))
		}
		return true
	}

	return false
}

export { endpointKey }
