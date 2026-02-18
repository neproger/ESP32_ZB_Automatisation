//UTF-8
//Groups.jsx
import { useCallback, useEffect, useMemo, useState } from 'react'
import { Link } from 'react-router-dom'
import { describeDeviceId } from '../zigbee/zcl.js'
import { useGateway } from '../gateway.jsx'
import EndpointWidgets from '../components/EndpointWidgets.jsx'
import {
	endpointLabelGet,
	groupMembersMap,
	groupsCreate,
	groupsDelete,
	groupsList,
	groupsReload,
	groupsRename,
	groupsSubscribe,
} from '../groupsStore.js'

function parseMemberKey(k) {
	const s = String(k ?? '')
	const parts = s.split('::')
	if (parts.length !== 2) return { uid: '', endpoint: 0 }
	return { uid: parts[0], endpoint: Number(parts[1] ?? 0) }
}

export default function Groups() {
	const { devices, deviceStates } = useGateway()
	const [name, setName] = useState('')
	const [status, setStatus] = useState('')
	const [groups, setGroups] = useState(() => groupsList())
	const [members, setMembers] = useState(() => groupMembersMap())

	useEffect(() => {
		const refresh = () => {
			setGroups(groupsList())
			setMembers(groupMembersMap())
		}
		const unsub = groupsSubscribe(refresh)
		groupsReload().catch(() => {})
		return () => unsub()
	}, [])

	const deviceByUid = useMemo(() => {
		const map = {}
		;(Array.isArray(devices) ? devices : []).forEach((d) => {
			const uid = String(d?.device_uid ?? '').trim().toLowerCase()
			if (uid) map[uid] = d
		})
		return map
	}, [devices])

	const groupTiles = useMemo(() => {
		const out = {}
		Object.entries(members || {}).forEach(([k, gid]) => {
			const groupId = String(gid ?? '')
			if (!groupId) return
			const { uid, endpoint } = parseMemberKey(k)
			if (!uid || endpoint <= 0) return

			const d = deviceByUid[String(uid).toLowerCase()]
			const ep = (Array.isArray(d?.endpoints) ? d.endpoints : []).find((e) => Number(e?.endpoint ?? 0) === endpoint)
			if (!d || !ep) return

			if (!out[groupId]) out[groupId] = []
			out[groupId].push({
				groupId,
				device: d,
				endpoint: ep,
				label: endpointLabelGet(uid, endpoint),
				sensors: Array.isArray(d?.sensors) ? d.sensors : [],
				liveStateByEndpoint: deviceStates?.[String(uid).toLowerCase()] || {},
			})
		})
		return out
	}, [members, deviceByUid, deviceStates])

	const onCreate = useCallback(async () => {
		try {
			const id = await groupsCreate(name)
			if (!id) {
				setStatus('Введите имя группы')
				return
			}
			setName('')
			setStatus('Группа создана')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [name])

	const onRename = useCallback(async (g) => {
		const cur = String(g?.name ?? '')
		const next = prompt('Новое имя группы:', cur)
		if (next == null) return
		try {
			await groupsRename(g?.id, next)
			setStatus('Группа переименована')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [])

	const onDelete = useCallback(async (g) => {
		const id = String(g?.id ?? '')
		if (!id) return
		if (!confirm(`Удалить группу "${String(g?.name ?? id)}"?`)) return
		try {
			await groupsDelete(id)
			setStatus('Группа удалена')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [])

	return (
		<div className="page">
			<div className="header">
				<div>
					<h1>Groups</h1>
					<div className="muted">Кастомные группы/комнаты. Endpoint-ы рендерятся теми же виджетами, что и в Device.</div>
				</div>
				<div className="row">
					<Link className="navlink" to="/devices">Back</Link>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div className="card" style={{ padding: 12, marginBottom: 12 }}>
				<div className="row">
					<input
						value={name}
						onChange={(e) => setName(String(e?.target?.value ?? ''))}
						placeholder="Новая группа (например, Гостиная)"
						style={{ minWidth: 280 }}
					/>
					<button onClick={onCreate}>Create group</button>
				</div>
			</div>

			<div className="groups-grid">
				{groups.map((g) => {
					const gid = String(g?.id ?? '')
					const items = groupTiles[gid] || []
					return (
						<div key={gid} className="card group-tile">
							<div className="group-tile-head">
								<div>
									<div className="group-title">{String(g?.name ?? gid)}</div>
									<div className="muted">{gid}</div>
								</div>
								<div className="row">
									<button onClick={() => onRename(g)}>Rename</button>
									<button onClick={() => onDelete(g)}>Delete</button>
								</div>
							</div>

							{items.length === 0 ? (
								<div className="muted">No endpoints in group.</div>
							) : (
								<div className="group-items">
									{items.map((item, idx) => {
										const endpointId = Number(item?.endpoint?.endpoint ?? 0)
										const devName = String(item?.device?.name ?? item?.device?.device_uid ?? '')
										const endpointLabel = String(item?.label ?? '').trim()
										const kindName = String(item?.endpoint?.kind ?? describeDeviceId(item?.endpoint?.device_id)?.name ?? '')
										return (
											<div key={`${gid}-${devName}-${endpointId}-${idx}`} className="endpoint-tile">
												<div className="endpoint-tile-head">
													<div className="endpoint-title">{endpointLabel || devName}</div>
													<div className="muted">EP{endpointId}{kindName ? ` · ${kindName}` : ''}</div>
												</div>
												<EndpointWidgets
													endpoint={item.endpoint}
													deviceUid={String(item.device?.device_uid ?? '')}
													liveStateByEndpoint={item.liveStateByEndpoint}
													sensors={item.sensors}
													onStatus={setStatus}
													compact
												/>
											</div>
										)
									})}
								</div>
							)}
						</div>
					)
				})}
			</div>
		</div>
	)
}
