import { Link, useParams } from 'react-router-dom'
import { useCallback, useMemo, useState } from 'react'
import { describeAttr, describeCluster, describeDeviceId, describeProfile, formatSensorValue, hex16 } from '../zigbee/zcl.js'
import { execAction } from '../api.js'
import { useGateway } from '../gateway.jsx'

function renderSensorValue(s) {
	return formatSensorValue(s)
}

function listToText(v) {
	return Array.isArray(v) ? v.map((x) => String(x ?? '')).filter(Boolean).join(', ') : ''
}

function clamp01(v) {
	if (!Number.isFinite(v)) return 0
	return Math.max(0, Math.min(1, v))
}

function srgbToLinear(v) {
	const x = v / 255
	return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4
}

function rgbHexToXy(hex) {
	const s = String(hex ?? '').replace('#', '').trim()
	if (s.length !== 6) return { x: 0, y: 0 }
	const r = parseInt(s.slice(0, 2), 16)
	const g = parseInt(s.slice(2, 4), 16)
	const b = parseInt(s.slice(4, 6), 16)
	if (![r, g, b].every((n) => Number.isFinite(n))) return { x: 0, y: 0 }

	// Convert sRGB -> linear -> XYZ (D65) -> xy (CIE 1931).
	const rl = srgbToLinear(r)
	const gl = srgbToLinear(g)
	const bl = srgbToLinear(b)

	const X = rl * 0.4124 + gl * 0.3576 + bl * 0.1805
	const Y = rl * 0.2126 + gl * 0.7152 + bl * 0.0722
	const Z = rl * 0.0193 + gl * 0.1192 + bl * 0.9505
	const sum = X + Y + Z
	const x = sum > 0 ? X / sum : 0
	const y = sum > 0 ? Y / sum : 0

	return {
		x: Math.round(clamp01(x) * 65535),
		y: Math.round(clamp01(y) * 65535),
	}
}

function linearToSrgbByte(v) {
	const x = Math.max(0, v)
	const s = x <= 0.0031308 ? (12.92 * x) : (1.055 * x ** (1 / 2.4) - 0.055)
	return Math.max(0, Math.min(255, Math.round(s * 255)))
}

function xyToRgbHex(xRaw, yRaw) {
	const x = Number(xRaw) / 65535
	const y = Number(yRaw) / 65535
	if (!Number.isFinite(x) || !Number.isFinite(y) || y <= 0) return '#ffffff'

	const Y = 1
	const X = (Y / y) * x
	const Z = (Y / y) * (1 - x - y)

	let rl = X * 3.2406 + Y * -1.5372 + Z * -0.4986
	let gl = X * -0.9689 + Y * 1.8758 + Z * 0.0415
	let bl = X * 0.0557 + Y * -0.204 + Z * 1.057

	const maxV = Math.max(rl, gl, bl, 1)
	rl /= maxV
	gl /= maxV
	bl /= maxV

	const r = linearToSrgbByte(rl)
	const g = linearToSrgbByte(gl)
	const b = linearToSrgbByte(bl)
	const hex = (n) => n.toString(16).padStart(2, '0')
	return `#${hex(r)}${hex(g)}${hex(b)}`
}

function toNumberOrNull(v) {
	const n = Number(v)
	return Number.isFinite(n) ? n : null
}

export default function Device() {
	const { uid } = useParams()
	const { devices, deviceStates, reloadDevices } = useGateway()
	const [levelByEndpoint, setLevelByEndpoint] = useState(() => new Map())
	const [colorByEndpoint, setColorByEndpoint] = useState(() => new Map())
	const [tempKByEndpoint, setTempKByEndpoint] = useState(() => new Map())
	const [status, setStatus] = useState('')

	const device = useMemo(() => {
		const u = String(uid ?? '')
		return (Array.isArray(devices) ? devices : []).find((d) => String(d?.device_uid ?? '') === u) || null
	}, [devices, uid])

	const endpoints = useMemo(() => (Array.isArray(device?.endpoints) ? device.endpoints : []), [device])
	const sensors = useMemo(() => (Array.isArray(device?.sensors) ? device.sensors : []), [device])
	const state = useMemo(() => ((device?.state && typeof device.state === 'object') ? device.state : {}), [device])

	const sortedEndpoints = useMemo(() => {
		const items = Array.isArray(endpoints) ? [...endpoints] : []
		items.sort((a, b) => Number(a?.endpoint ?? 0) - Number(b?.endpoint ?? 0))
		return items
	}, [endpoints])

	const liveStateByEndpoint = useMemo(() => {
		const u = String(uid ?? '')
		return (u && deviceStates?.[u]) ? deviceStates[u] : {}
	}, [uid, deviceStates])

	const sortedSensors = useMemo(() => {
		const items = Array.isArray(sensors) ? [...sensors] : []
		items.sort((a, b) => {
			const ak = `${a?.endpoint ?? 0}:${a?.cluster_id ?? 0}:${a?.attr_id ?? 0}`
			const bk = `${b?.endpoint ?? 0}:${b?.cluster_id ?? 0}:${b?.attr_id ?? 0}`
			return ak.localeCompare(bk)
		})
		return items
	}, [sensors])

	const getSensorValue = useCallback(
		(endpoint, clusterId, attrId) => {
			const ep = Number(endpoint)
			const item = sortedSensors.find(
				(s) =>
					Number(s?.endpoint ?? 0) === ep &&
					Number(s?.cluster_id ?? 0) === Number(clusterId) &&
					Number(s?.attr_id ?? 0) === Number(attrId),
			)
			if (!item) return null
			if (item?.value_u32 != null) return toNumberOrNull(item.value_u32)
			if (item?.value_i32 != null) return toNumberOrNull(item.value_i32)
			return null
		},
		[sortedSensors],
	)

	const getEndpointState = useCallback(
		(endpoint) => {
			const ep = String(Number(endpoint ?? 0))
			return {
				...(state && typeof state === 'object' ? state : {}),
				...((liveStateByEndpoint && liveStateByEndpoint['0']) || {}),
				...((liveStateByEndpoint && liveStateByEndpoint[ep]) || {}),
			}
		},
		[liveStateByEndpoint, state],
	)

	const getLevelValue = useCallback(
		(endpoint) => {
			const ep = Number(endpoint)
			if (levelByEndpoint.has(ep)) return Number(levelByEndpoint.get(ep))
			const st = getEndpointState(ep)
			const fromState = toNumberOrNull(st?.level)
			if (fromState != null) return Math.max(0, Math.min(254, Math.round(fromState)))
			const fromSensor = getSensorValue(ep, 0x0008, 0x0000)
			if (fromSensor != null) return Math.max(0, Math.min(254, Math.round(fromSensor)))
			return 0
		},
		[levelByEndpoint, getEndpointState, getSensorValue],
	)

	const getTempKValue = useCallback(
		(endpoint) => {
			const ep = Number(endpoint)
			if (tempKByEndpoint.has(ep)) return Number(tempKByEndpoint.get(ep))
			const st = getEndpointState(ep)
			const miredFromState = toNumberOrNull(st?.color_temp_mireds)
			const miredFromSensor = getSensorValue(ep, 0x0300, 0x0007)
			const mired = miredFromState ?? miredFromSensor
			if (mired != null && mired > 0) {
				return Math.max(2000, Math.min(6500, Math.round(1_000_000 / mired)))
			}
			return 3000
		},
		[tempKByEndpoint, getEndpointState, getSensorValue],
	)

	const getColorValue = useCallback(
		(endpoint) => {
			const ep = Number(endpoint)
			if (colorByEndpoint.has(ep)) return String(colorByEndpoint.get(ep))
			const st = getEndpointState(ep)
			const x = toNumberOrNull(st?.color_x) ?? getSensorValue(ep, 0x0300, 0x0003)
			const y = toNumberOrNull(st?.color_y) ?? getSensorValue(ep, 0x0300, 0x0004)
			if (x != null && y != null) return xyToRgbHex(x, y)
			return '#ffffff'
		},
		[colorByEndpoint, getEndpointState, getSensorValue],
	)

	const hasAccept = useCallback((ep, prefix) => {
		const arr = Array.isArray(ep?.accepts) ? ep.accepts : []
		return arr.some((x) => String(x ?? '').startsWith(prefix))
	}, [])

	const sendOnOff = useCallback(
		async (endpoint, cmd) => {
			const u = String(uid ?? '')
			if (!u) return
			setStatus('Sending...')
			try {
				await execAction({
					type: 'zigbee',
					cmd: `onoff.${cmd}`,
					device_uid: u,
					endpoint,
				})
				setStatus('')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[uid],
	)

	const sendLevel = useCallback(
		async (endpoint) => {
			const u = String(uid ?? '')
			const level = getLevelValue(endpoint)
			if (!u) return
			if (!Number.isFinite(level)) return
			setStatus('Sending...')
			try {
				await execAction({
					type: 'zigbee',
					cmd: 'level.move_to_level',
					device_uid: u,
					endpoint,
					level: Math.max(0, Math.min(254, Math.round(level))),
					transition_ms: 300,
				})
				setStatus('')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[uid, getLevelValue],
	)

	const sendColor = useCallback(
		async (endpoint) => {
			const u = String(uid ?? '')
			const hex = String(getColorValue(endpoint))
			if (!u) return
			const { x, y } = rgbHexToXy(hex)
			setStatus('Sending...')
			try {
				await execAction({
					type: 'zigbee',
					cmd: 'color.move_to_color_xy',
					device_uid: u,
					endpoint,
					x,
					y,
					transition_ms: 400,
				})
				setStatus('')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[uid, getColorValue],
	)

	const sendTemp = useCallback(
		async (endpoint) => {
			const u = String(uid ?? '')
			const k = Number(getTempKValue(endpoint))
			if (!u || !Number.isFinite(k) || k <= 0) return
			const mireds = Math.round(1_000_000 / k)
			setStatus('Sending...')
			try {
				await execAction({
					type: 'zigbee',
					cmd: 'color.move_to_color_temperature',
					device_uid: u,
					endpoint,
					mireds,
					transition_ms: 600,
				})
				setStatus('')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[uid, getTempKValue],
	)

	return (
		<div className="page">
			<div className="header">
				<div>
					<h1>Device</h1>
					<div className="muted">
						{device?.name ? (
							<>
								<span>{String(device.name)}</span> <span className="muted">·</span>{' '}
							</>
						) : null}
						<code>{String(uid ?? '')}</code>
					</div>
				</div>
				<div className="row">
					<button onClick={reloadDevices}>Refresh</button>
					<Link className="navlink" to="/devices">
						Back
					</Link>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div className="card">
				<div className="table-wrap">
					<table>
						<thead>
							<tr>
								<th>Endpoint</th>
								<th>Kind</th>
								<th>Profile</th>
								<th>Device</th>
								<th>Controls</th>
								<th>Accepts</th>
								<th>Emits</th>
								<th>Reports</th>
								<th>In clusters</th>
								<th>Out clusters</th>
							</tr>
						</thead>
						<tbody>
							{sortedEndpoints.length === 0 ? (
								<tr>
									<td colSpan={10} className="muted">
										No endpoints discovered yet.
									</td>
								</tr>
							) : (
								sortedEndpoints.map((e, index) => (
									<tr key={index}>
										<td>
											<code>{String(e?.endpoint ?? '')}</code>
										</td>
										<td className="muted">{String(e?.kind ?? '')}</td>
										<td className="muted">
											{hex16(e?.profile_id)}
											{describeProfile(e?.profile_id)?.name ? ` (${describeProfile(e?.profile_id).name})` : ''}
										</td>
										<td className="muted">
											{hex16(e?.device_id)}
											{describeDeviceId(e?.device_id)?.name ? ` (${describeDeviceId(e?.device_id).name})` : ''}
										</td>
										<td>
											<div className="row" style={{ flexWrap: 'wrap', gap: 8 }}>
												{hasAccept(e, 'onoff.') ? (
													<>
														<label className="muted" style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
															<input
																type="checkbox"
																checked={Boolean(
																	liveStateByEndpoint?.[String(Number(e?.endpoint ?? 0))]?.onoff ??
																	liveStateByEndpoint?.['0']?.onoff ??
																	state?.onoff
																)}
																onChange={(ev) => sendOnOff(Number(e?.endpoint ?? 1), ev?.target?.checked ? 'on' : 'off')}
															/>
															On
														</label>
														<button onClick={() => sendOnOff(Number(e?.endpoint ?? 1), 'toggle')}>Toggle</button>
													</>
												) : null}

												{hasAccept(e, 'level.move_to_level') || hasAccept(e, 'level.') ? (
													<>
														<input
															type="range"
															min={0}
															max={254}
															value={getLevelValue(Number(e?.endpoint ?? 0))}
															onChange={(ev) => {
																const v = Number(ev?.target?.value ?? 0)
																setLevelByEndpoint((prev) => {
																	const next = new Map(prev)
																	next.set(Number(e?.endpoint ?? 0), v)
																	return next
																})
															}}
														/>
														<button onClick={() => sendLevel(Number(e?.endpoint ?? 1))}>Set</button>
													</>
												) : null}

												{hasAccept(e, 'color.move_to_color_xy') || hasAccept(e, 'color.') ? (
													<>
														<input
															type="color"
															value={getColorValue(Number(e?.endpoint ?? 0))}
															onChange={(ev) => {
																const v = String(ev?.target?.value ?? '#ffffff')
																setColorByEndpoint((prev) => {
																	const next = new Map(prev)
																	next.set(Number(e?.endpoint ?? 0), v)
																	return next
																})
															}}
															title="Color (xy)"
														/>
														<button onClick={() => sendColor(Number(e?.endpoint ?? 1))}>Set</button>
													</>
												) : null}

												{hasAccept(e, 'color.move_to_color_temperature') ? (
													<>
														<input
															type="range"
															min={2000}
															max={6500}
															step={100}
															value={getTempKValue(Number(e?.endpoint ?? 0))}
															onChange={(ev) => {
																const v = Number(ev?.target?.value ?? 3000)
																setTempKByEndpoint((prev) => {
																	const next = new Map(prev)
																	next.set(Number(e?.endpoint ?? 0), v)
																	return next
																})
															}}
															title="Color temperature (K)"
														/>
														<button onClick={() => sendTemp(Number(e?.endpoint ?? 1))}>Set</button>
													</>
												) : null}
											</div>
										</td>
										<td className="mono" style={{ flexWrap: 'wrap' }}>{listToText(e?.accepts)}</td>
										<td className="mono" style={{ flexWrap: 'wrap' }}>{listToText(e?.emits)}</td>
										<td className="mono">{listToText(e?.reports)}</td>
										<td className="mono">
											{Array.isArray(e?.in_clusters)
												? e.in_clusters
													.map((c) => <div key={c}>{`${hex16(c)}${describeCluster(c)?.name ? ` ${describeCluster(c).name}` : ''}`}</div>)
												: ''}
										</td>
										<td className="mono">
											{Array.isArray(e?.out_clusters)
												? e.out_clusters
													.map((c) => <div key={c}>{`${hex16(c)}${describeCluster(c)?.name ? ` ${describeCluster(c).name}` : ''}`}</div>)
												: ''}
										</td>
									</tr>
								))
							)}
						</tbody>
					</table>
				</div>
			</div>

			<div style={{ height: 12 }} />

			<div className="card">
				<div className="table-wrap">
					<table>
						<thead>
							<tr>
								<th>Endpoint</th>
								<th>Cluster</th>
								<th>Attr</th>
								<th>Value</th>
								<th>ts_ms</th>
							</tr>
						</thead>
						<tbody>
							{sortedSensors.length === 0 ? (
								<tr>
									<td colSpan={5} className="muted">
										No sensor values yet (wait for reports or initial reads).
									</td>
								</tr>
							) : (
								sortedSensors.map((s, idx) => (
									<tr key={`${idx}-${s?.endpoint}-${s?.cluster_id}-${s?.attr_id}`}>
										<td>
											<code>{String(s?.endpoint ?? '')}</code>
										</td>
										<td className="muted">
											{hex16(s?.cluster_id)}
											{describeCluster(s?.cluster_id)?.name ? ` (${describeCluster(s?.cluster_id).name})` : ''}
										</td>
										<td className="muted">
											{hex16(s?.attr_id)}
											{describeAttr(s?.cluster_id, s?.attr_id)?.name ? ` (${describeAttr(s?.cluster_id, s?.attr_id).name})` : ''}
										</td>
										<td className="mono">{renderSensorValue(s)}</td>
										<td className="muted">{String(s?.ts_ms ?? '')}</td>
									</tr>
								))
							)}
						</tbody>
					</table>
				</div>
			</div>
		</div>
	)
}
