//UTF-8
//Devices.jsx
import { Link } from 'react-router-dom'
import { useCallback, useMemo, useState } from 'react'
import { useGateway } from '../gateway.jsx'

function capsToText(device) {
	const caps = []
	if (device?.has_onoff) caps.push('onoff')
	if (device?.has_button) caps.push('button')
	return caps.join(', ')
}

function shortToHex(shortAddr) {
	const v = Number(shortAddr ?? 0)
	if (!Number.isFinite(v)) return '0x0'
	return `0x${v.toString(16)}`
}

export default function Devices() {
	const { devices, reloadDevices, wsStatus, permitJoin, renameDevice, removeDevice, removeAllDevices } = useGateway()
	const [loading, setLoading] = useState(false)
	const [status, setStatus] = useState('')

	const sortedDevices = useMemo(() => {
		const items = Array.isArray(devices) ? [...devices] : []
		items.sort((a, b) => String(a?.device_uid ?? '').localeCompare(String(b?.device_uid ?? '')))
		return items
	}, [devices])

	const loadDevices = useCallback(async () => {
		setLoading(true)
		setStatus('')
		try {
			await reloadDevices()
		} catch (e) {
			setStatus(String(e?.message ?? e))
		} finally {
			setLoading(false)
		}
	}, [reloadDevices])

	const permitJoinRequest = useCallback(async () => {
		setStatus('Permit join: requesting...')
		try {
			await permitJoin(180)
			setStatus('Permit join command sent')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [permitJoin])

	const removeDeviceRequest = useCallback(
		async (uid) => {
			const u = String(uid ?? '')
			if (!u) return
			if (!confirm(`Удалить устройство ${u} из памяти шлюза?`)) return

			setStatus('Удаление...')
			try {
				await removeDevice(u)
				setStatus('Запрос на удаление отправлен (ждем событие обновления)')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[removeDevice],
	)

	const removeAllDevicesRequest = useCallback(async () => {
		if (!confirm('Удалить все Zigbee-устройства из шлюза?')) return

		setStatus('Удаление всех устройств...')
		try {
			await removeAllDevices()
			setStatus('Запрос на удаление всех устройств отправлен')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [removeAllDevices])

	const renameDeviceRequest = useCallback(
		async (uid, currentName) => {
			const u = String(uid ?? '')
			if (!u) return
			const next = prompt(`Device name for ${u}:`, String(currentName ?? ''))
			if (next === null) return

			setStatus('Renaming...')
			try {
				await renameDevice(u, String(next))
				setStatus('Renamed (waiting WS sync)')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[renameDevice],
	)

	return (
		<div className="page">
			<div className="header">
				<div>
					<h1>Devices</h1>
					<div className="muted">Zigbee devices that joined/rejoined (DEVICE_ANNCE).</div>
				</div>
				<div className="row">
					<button onClick={loadDevices} disabled={loading}>
						{loading ? 'Refreshing...' : 'Refresh'}
					</button>
					<button onClick={permitJoinRequest}>Scan new devices (permit join)</button>
					<button onClick={removeAllDevicesRequest}>Удалить все</button>
					<div className="muted">ws: {wsStatus}</div>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div className="card">
				<div className="table-wrap">
					<table>
						<thead>
							<tr>
								<th>UID</th>
								<th>Name</th>
								<th>Short</th>
								<th>Caps</th>
								<th>Actions</th>
							</tr>
						</thead>
						<tbody>
							{sortedDevices.length === 0 ? (
								<tr>
									<td colSpan={5} className="muted">
										No devices yet. Click "Scan new devices (permit join)", then pair a Zigbee device.
									</td>
								</tr>
							) : (
								sortedDevices.map((d) => (
									<tr key={String(d?.device_uid ?? '')}>
										<td>
											<Link to={`/devices/${encodeURIComponent(String(d?.device_uid ?? ''))}`}>
												<code>{String(d?.device_uid ?? '')}</code>
											</Link>
										</td>
										<td>{String(d?.name ?? '')}</td>
										<td>
											<code>{shortToHex(d?.short_addr)}</code>
										</td>
										<td>{capsToText(d)}</td>
										<td>
											<div className="row">
												<button onClick={() => renameDeviceRequest(d?.device_uid, d?.name)}>Rename</button>
												<button onClick={() => removeDeviceRequest(d?.device_uid)}>Удалить</button>
											</div>
										</td>
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
