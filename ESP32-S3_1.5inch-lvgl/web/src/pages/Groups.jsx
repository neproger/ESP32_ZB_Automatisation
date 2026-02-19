//UTF-8
//Groups.jsx
import { useCallback, useEffect, useMemo, useState } from 'react'
import { Link } from 'react-router-dom'
import { describeDeviceId } from '../zigbee/zcl.js'
import { useGateway } from '../gateway.jsx'
import EndpointWidgets from '../components/EndpointWidgets.jsx'
import {
	endpointLabelGet,
	groupItemsList,
	groupReorder,
	groupsCreate,
	groupsDelete,
	groupsList,
	groupsReload,
	groupsRename,
	groupsSubscribe,
} from '../groupsStore.js'

export default function Groups() {
	const { devices, deviceStates } = useGateway()
	const [name, setName] = useState('')
	const [status, setStatus] = useState('')
	const [groups, setGroups] = useState(() => groupsList())
	const [items, setItems] = useState(() => groupItemsList())
	const [localOrder, setLocalOrder] = useState({})
	const [savingGroupId, setSavingGroupId] = useState('')

	useEffect(() => {
		const refresh = () => {
			setGroups(groupsList())
			setItems(groupItemsList())
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

	const backendGroupTiles = useMemo(() => {
		const out = {}
		const list = Array.isArray(items) ? [...items] : []
		list.sort((a, b) => Number(a?.order ?? 0) - Number(b?.order ?? 0))
		list.forEach((it) => {
			const groupId = String(it?.group_id ?? '')
			const uid = String(it?.device_uid ?? '').trim().toLowerCase()
			const endpoint = Number(it?.endpoint_id ?? 0)
			if (!groupId || !uid || endpoint <= 0) return

			const d = deviceByUid[uid]
			const ep = (Array.isArray(d?.endpoints) ? d.endpoints : []).find((e) => Number(e?.endpoint ?? 0) === endpoint)
			if (!d || !ep) return

			if (!out[groupId]) out[groupId] = []
			out[groupId].push({
				groupId,
				device_uid: uid,
				endpoint_id: endpoint,
				order: Number(it?.order ?? 0),
				device: d,
				endpoint: ep,
				label: endpointLabelGet(uid, endpoint),
				sensors: Array.isArray(d?.sensors) ? d.sensors : [],
				liveStateByEndpoint: deviceStates?.[uid] || {},
			})
		})
		return out
	}, [items, deviceByUid, deviceStates])

	const groupTiles = useMemo(() => {
		const out = {}
		Object.keys(backendGroupTiles).forEach((gid) => {
			const fromLocal = localOrder[gid]
			out[gid] = Array.isArray(fromLocal) && fromLocal.length > 0 ? fromLocal : backendGroupTiles[gid]
		})
		return out
	}, [backendGroupTiles, localOrder])

	const reorderLocal = useCallback((gid, fromIdx, toIdx) => {
		setLocalOrder((prev) => {
			const current = Array.isArray(prev?.[gid]) && prev[gid].length > 0
				? [...prev[gid]]
				: [...(backendGroupTiles[gid] || [])]
			if (fromIdx < 0 || fromIdx >= current.length || toIdx < 0 || toIdx >= current.length) {
				return prev
			}
			const [moved] = current.splice(fromIdx, 1)
			current.splice(toIdx, 0, moved)
			return { ...prev, [gid]: current }
		})
	}, [backendGroupTiles])

	const persistOrder = useCallback(async (gid) => {
		const ordered = localOrder?.[gid]
		if (!Array.isArray(ordered) || ordered.length === 0) return
		setSavingGroupId(gid)
		try {
			await groupReorder(gid, ordered)
			setStatus('Порядок виджетов сохранен')
			setLocalOrder((prev) => {
				const next = { ...(prev || {}) }
				delete next[gid]
				return next
			})
		} catch (e) {
			setStatus(String(e?.message ?? e))
		} finally {
			setSavingGroupId('')
		}
	}, [localOrder])

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
					const hasLocalChanges = Array.isArray(localOrder?.[gid]) && localOrder[gid].length > 0
					const isSaving = savingGroupId === gid
					return (
						<div key={gid} className="card group-tile">
							<div className="group-tile-head">
								<div>
									<div className="group-title">{String(g?.name ?? gid)}</div>
									<div className="muted">{gid}</div>
								</div>
								<div className="row">
									<button disabled={!hasLocalChanges || isSaving} onClick={() => persistOrder(gid)}>
										{isSaving ? 'Saving...' : 'Save order'}
									</button>
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
													<div className="row endpoint-title-row">
														<div>
															<div className="endpoint-title">{endpointLabel || devName}</div>
															<div className="muted">EP{endpointId}{kindName ? ` · ${kindName}` : ''}</div>
														</div>
														<div className="row">
															<button disabled={idx <= 0} onClick={() => reorderLocal(gid, idx, idx - 1)}>↑</button>
															<button disabled={idx >= items.length - 1} onClick={() => reorderLocal(gid, idx, idx + 1)}>↓</button>
														</div>
													</div>
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
