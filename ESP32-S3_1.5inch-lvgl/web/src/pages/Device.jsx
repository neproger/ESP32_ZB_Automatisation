//UTF-8
//Device.jsx
import { Link, useParams } from 'react-router-dom'
import { useCallback, useEffect, useMemo, useState } from 'react'
import { describeCluster, describeDeviceId, describeProfile, hex16 } from '../zigbee/zcl.js'
import { useGateway } from '../gateway.jsx'
import EndpointWidgets from '../components/EndpointWidgets.jsx'
import {
	endpointLabelGet,
	endpointLabelSet,
	groupGetForEndpoint,
	groupSetForEndpoint,
	groupsList,
	groupsReload,
	groupsSubscribe,
} from '../groupsStore.js'

function normalizeUid(v) {
	return String(v ?? '').trim().toLowerCase()
}

function listToLines(v) {
	return Array.isArray(v) ? v.map((x) => String(x ?? '').trim()).filter(Boolean) : []
}

function formatStateValue(v) {
	if (typeof v === 'boolean') return v ? 'true' : 'false'
	if (typeof v === 'number') return Number.isFinite(v) ? String(v) : '-'
	if (typeof v === 'string') return v
	if (v == null) return '-'
	try {
		return JSON.stringify(v)
	} catch {
		return String(v)
	}
}

async function copyTextToClipboard(text) {
	if (navigator?.clipboard?.writeText) {
		await navigator.clipboard.writeText(text)
		return
	}
	const ta = document.createElement('textarea')
	ta.value = text
	ta.setAttribute('readonly', '')
	ta.style.position = 'absolute'
	ta.style.left = '-9999px'
	document.body.appendChild(ta)
	ta.select()
	document.execCommand('copy')
	document.body.removeChild(ta)
}

const ENDPOINT_COLUMNS = [
	{ id: 'endpoint', label: 'Endpoint' },
	{ id: 'kind', label: 'Kind' },
	{ id: 'profile', label: 'Profile' },
	{ id: 'device', label: 'Device' },
	{ id: 'controls', label: 'Controls' },
	{ id: 'accepts', label: 'Accepts' },
	{ id: 'emits', label: 'Emits' },
	{ id: 'reports', label: 'Reports' },
	{ id: 'live_state', label: 'Live state' },
	{ id: 'group', label: 'Group' },
	{ id: 'in_clusters', label: 'In clusters' },
	{ id: 'out_clusters', label: 'Out clusters' },
]

const DEVICE_COLUMNS_KEY = 'gw_device_columns_v1'
const DEFAULT_COLUMN_VISIBILITY = {
	endpoint: true,
	kind: true,
	profile: true,
	device: true,
	controls: true,
	accepts: false,
	emits: false,
	reports: false,
	live_state: true,
	group: true,
	in_clusters: false,
	out_clusters: false,
}

function loadColumnVisibility() {
	try {
		const raw = window.localStorage.getItem(DEVICE_COLUMNS_KEY)
		if (!raw) return { ...DEFAULT_COLUMN_VISIBILITY }
		const parsed = JSON.parse(raw)
		if (!parsed || typeof parsed !== 'object') return { ...DEFAULT_COLUMN_VISIBILITY }
		return { ...DEFAULT_COLUMN_VISIBILITY, ...parsed }
	} catch {
		return { ...DEFAULT_COLUMN_VISIBILITY }
	}
}

export default function Device() {
	const { uid } = useParams()
	const { devices, deviceStates, reloadDevices } = useGateway()
	const [status, setStatus] = useState('')
	const [columnsOpen, setColumnsOpen] = useState(false)
	const [columnVisible, setColumnVisible] = useState(loadColumnVisibility)
	const [groups, setGroups] = useState(() => groupsList())
	const [labelDraftByEndpoint, setLabelDraftByEndpoint] = useState({})

	useEffect(() => {
		try {
			window.localStorage.setItem(DEVICE_COLUMNS_KEY, JSON.stringify(columnVisible))
		} catch {
			// ignore storage errors
		}
	}, [columnVisible])

	useEffect(() => {
		const refresh = () => setGroups(groupsList())
		const unsub = groupsSubscribe(refresh)
		groupsReload().catch(() => {})
		return () => unsub()
	}, [])

	const device = useMemo(() => {
		const u = normalizeUid(uid)
		return (Array.isArray(devices) ? devices : []).find((d) => normalizeUid(d?.device_uid) === u) || null
	}, [devices, uid])

	const endpoints = useMemo(() => (Array.isArray(device?.endpoints) ? device.endpoints : []), [device])
	const sensors = useMemo(() => (Array.isArray(device?.sensors) ? device.sensors : []), [device])
	const sortedEndpoints = useMemo(() => {
		const items = [...endpoints]
		items.sort((a, b) => Number(a?.endpoint ?? 0) - Number(b?.endpoint ?? 0))
		return items
	}, [endpoints])

	const liveStateByEndpoint = useMemo(() => {
		const u = normalizeUid(uid)
		return (u && deviceStates?.[u]) ? deviceStates[u] : {}
	}, [uid, deviceStates])

	useEffect(() => {
		const duid = String(device?.device_uid ?? uid ?? '')
		const next = {}
		;(Array.isArray(device?.endpoints) ? device.endpoints : []).forEach((e) => {
			const ep = Number(e?.endpoint ?? 0)
			if (ep <= 0) return
			next[String(ep)] = endpointLabelGet(duid, ep)
		})
		setLabelDraftByEndpoint(next)
	}, [device, uid, groups])

	const copyJson = useCallback(async (label, data) => {
		try {
			await copyTextToClipboard(JSON.stringify(data, null, 2))
			setStatus(`${label} JSON copied`)
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [])

	const copyEndpointJson = useCallback(async (e) => {
		const endpointId = Number(e?.endpoint ?? 0)
		const payload = {
			device_uid: String(device?.device_uid ?? uid ?? ''),
			device_name: String(device?.name ?? ''),
			short_addr: Number(device?.short_addr ?? 0),
			endpoint: endpointId,
			meta: e ?? {},
			label: String(labelDraftByEndpoint[String(endpointId)] ?? ''),
			live_state: (liveStateByEndpoint && liveStateByEndpoint[String(endpointId)]) || {},
			sensors: sensors.filter((s) => Number(s?.endpoint ?? 0) === endpointId),
		}
		await copyJson(`Endpoint ${endpointId}`, payload)
	}, [copyJson, device, uid, liveStateByEndpoint, sensors, labelDraftByEndpoint])

	const copyDeviceJson = useCallback(async () => {
		await copyJson('Device', {
			device: device ?? null,
			live_state: liveStateByEndpoint ?? {},
			endpoints: sortedEndpoints.map((e) => ({
				...(e ?? {}),
				label: String(labelDraftByEndpoint[String(Number(e?.endpoint ?? 0))] ?? ''),
				live_state: (liveStateByEndpoint && liveStateByEndpoint[String(Number(e?.endpoint ?? 0))]) || {},
				sensors: sensors.filter((s) => Number(s?.endpoint ?? 0) === Number(e?.endpoint ?? 0)),
			})),
			sensors,
		})
	}, [copyJson, device, liveStateByEndpoint, sortedEndpoints, sensors, labelDraftByEndpoint])

	const showCol = useCallback((id) => columnVisible[id] !== false, [columnVisible])
	const endpointColSpan = useMemo(() => ENDPOINT_COLUMNS.filter((c) => showCol(c.id)).length, [showCol])

	const endpointGroupId = useCallback((endpoint) => {
		const duid = String(device?.device_uid ?? uid ?? '')
		return groupGetForEndpoint(duid, Number(endpoint ?? 0))
	}, [device, uid])

	const onEndpointGroupChange = useCallback(async (endpoint, groupId) => {
		const duid = String(device?.device_uid ?? uid ?? '')
		try {
			await groupSetForEndpoint(duid, Number(endpoint ?? 0), groupId)
			setStatus('Группа endpoint обновлена')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [device, uid])

	const onEndpointLabelChange = useCallback((endpoint, value) => {
		setLabelDraftByEndpoint((prev) => ({
			...prev,
			[String(Number(endpoint ?? 0))]: String(value ?? ''),
		}))
	}, [])

	const saveEndpointLabel = useCallback(async (endpoint) => {
		const duid = String(device?.device_uid ?? uid ?? '')
		const ep = Number(endpoint ?? 0)
		if (!duid || ep <= 0) return
		const nextValue = String(labelDraftByEndpoint[String(ep)] ?? '').trim()
		const prevValue = String(endpointLabelGet(duid, ep) ?? '').trim()
		if (nextValue === prevValue) return
		try {
			await endpointLabelSet(duid, ep, nextValue)
			setStatus('Имя endpoint сохранено')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [device, uid, labelDraftByEndpoint])

	return (
		<div className="page">
			<div className="header">
				<div>
					<h1>Device</h1>
					<div className="muted">
						{device?.name ? <><span>{String(device.name)}</span> <span className="muted">·</span> </> : null}
						<code>{String(uid ?? '')}</code>
					</div>
				</div>
				<div className="row">
					<button onClick={reloadDevices}>Refresh</button>
					<button onClick={copyDeviceJson}>Copy Device JSON</button>
					<div style={{ position: 'relative' }}>
						<button onClick={() => setColumnsOpen((v) => !v)}>Columns</button>
						{columnsOpen ? (
							<div className="card" style={{ position: 'absolute', right: 0, top: '110%', zIndex: 20, padding: 10, minWidth: 220 }}>
								{ENDPOINT_COLUMNS.map((c) => (
									<label key={c.id} className="row" style={{ display: 'flex', gap: 8, marginBottom: 6 }}>
										<input
											type="checkbox"
											checked={showCol(c.id)}
											onChange={(ev) =>
												setColumnVisible((prev) => ({
													...prev,
													[c.id]: Boolean(ev?.target?.checked),
												}))
											}
										/>
										<span>{c.label}</span>
									</label>
								))}
							</div>
						) : null}
					</div>
					<Link className="navlink" to="/devices">Back</Link>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div className="card">
				<div className="table-wrap">
					<table>
						<thead>
							<tr>
								{showCol('endpoint') ? <th>Endpoint</th> : null}
								{showCol('kind') ? <th>Kind</th> : null}
								{showCol('profile') ? <th>Profile</th> : null}
								{showCol('device') ? <th>Device</th> : null}
								{showCol('controls') ? <th>Controls</th> : null}
								{showCol('accepts') ? <th>Accepts</th> : null}
								{showCol('emits') ? <th>Emits</th> : null}
								{showCol('reports') ? <th>Reports</th> : null}
								{showCol('live_state') ? <th>Live state</th> : null}
								{showCol('group') ? <th>Group</th> : null}
								{showCol('in_clusters') ? <th>In clusters</th> : null}
								{showCol('out_clusters') ? <th>Out clusters</th> : null}
							</tr>
						</thead>
						<tbody>
							{sortedEndpoints.length === 0 ? (
								<tr>
									<td colSpan={endpointColSpan} className="muted">No endpoints discovered yet.</td>
								</tr>
							) : (
								sortedEndpoints.map((e, index) => {
									const endpointId = Number(e?.endpoint ?? 0)
									const endpointState = (liveStateByEndpoint && liveStateByEndpoint[String(endpointId)]) || {}
									const endpointLabel = String(labelDraftByEndpoint[String(endpointId)] ?? '').trim()
									return (
										<tr key={index}>
											{showCol('endpoint') ? (
												<td>
													{endpointLabel ? <div style={{ marginBottom: 6, fontWeight: 600 }}>{endpointLabel}</div> : null}
													<code>{String(e?.endpoint ?? '')}</code>
													<div style={{ marginTop: 6 }}>
														<button onClick={() => copyEndpointJson(e)}>Copy JSON</button>
													</div>
												</td>
											) : null}
											{showCol('kind') ? <td className="muted">{String(e?.kind ?? '')}</td> : null}
											{showCol('profile') ? (
												<td className="muted">
													{hex16(e?.profile_id)}
													{describeProfile(e?.profile_id)?.name ? ` (${describeProfile(e?.profile_id).name})` : ''}
												</td>
											) : null}
											{showCol('device') ? (
												<td className="muted">
													{hex16(e?.device_id)}
													{describeDeviceId(e?.device_id)?.name ? ` (${describeDeviceId(e?.device_id).name})` : ''}
												</td>
											) : null}
											{showCol('controls') ? (
												<td>
													<EndpointWidgets
														endpoint={e}
														deviceUid={String(device?.device_uid ?? uid ?? '')}
														liveStateByEndpoint={liveStateByEndpoint}
														sensors={sensors}
														onStatus={setStatus}
													/>
												</td>
											) : null}
											{showCol('accepts') ? <td className="mono">{listToLines(e?.accepts).map((a) => <div key={a}>{a}</div>)}</td> : null}
											{showCol('emits') ? <td className="mono">{listToLines(e?.emits).map((a) => <div key={a}>{a}</div>)}</td> : null}
											{showCol('reports') ? <td className="mono">{listToLines(e?.reports).map((a) => <div key={a}>{a}</div>)}</td> : null}
											{showCol('live_state') ? (
												<td className="mono">
													{Object.entries(endpointState).length === 0
														? '-'
														: Object.entries(endpointState).map(([k, v]) => <div key={k}>{`${k}: ${formatStateValue(v)}`}</div>)}
												</td>
											) : null}
											{showCol('group') ? (
												<td>
													<input
														value={String(labelDraftByEndpoint[String(endpointId)] ?? '')}
														onChange={(ev) => onEndpointLabelChange(endpointId, ev?.target?.value)}
														onBlur={() => saveEndpointLabel(endpointId)}
														placeholder="Endpoint name"
														style={{ marginBottom: 8, minWidth: 160 }}
													/>
													<select
														value={endpointGroupId(endpointId)}
														onChange={(ev) => onEndpointGroupChange(endpointId, String(ev?.target?.value ?? ''))}
													>
														<option value="">- no group -</option>
														{groups.map((g) => (
															<option key={String(g?.id ?? '')} value={String(g?.id ?? '')}>
																{String(g?.name ?? g?.id ?? '')}
															</option>
														))}
													</select>
												</td>
											) : null}
											{showCol('in_clusters') ? (
												<td className="mono">
													{Array.isArray(e?.in_clusters)
														? e.in_clusters.map((c) => <div key={c}>{`${hex16(c)}${describeCluster(c)?.name ? ` ${describeCluster(c).name}` : ''}`}</div>)
														: ''}
												</td>
											) : null}
											{showCol('out_clusters') ? (
												<td className="mono">
													{Array.isArray(e?.out_clusters)
														? e.out_clusters.map((c) => <div key={c}>{`${hex16(c)}${describeCluster(c)?.name ? ` ${describeCluster(c).name}` : ''}`}</div>)
														: ''}
												</td>
											) : null}
										</tr>
									)
								})
							)}
						</tbody>
					</table>
				</div>
			</div>
		</div>
	)
}
