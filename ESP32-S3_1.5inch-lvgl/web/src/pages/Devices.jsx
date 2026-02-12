//UTF-8
//Devices.jsx
import { Link } from 'react-router-dom'
import { useCallback, useMemo, useState } from 'react'
import { postCbor } from '../api.js'
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
	const { devices, reloadDevices, wsStatus } = useGateway()
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

	const permitJoin = useCallback(async () => {
		setStatus('Permit join: requesting...')
		try {
			const data = await postCbor('/api/network/permit_join', { seconds: 180 })
			const seconds = Number(data?.seconds ?? 180)
			setStatus(`Permit join enabled for ${Number.isFinite(seconds) ? seconds : 180}s`)
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [])

	const removeDevice = useCallback(
		async (uid) => {
			const u = String(uid ?? '')
			if (!u) return
			if (!confirm(`Удалить устройство ${u} из памяти шлюза?`)) return

			setStatus('Удаление...')
			try {
				await postCbor('/api/devices/remove', { device_uid: u })
				setStatus('Запрос на удаление отправлен (ждем событие обновления)')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[],
	)

	const renameDevice = useCallback(
		async (uid, currentName) => {
			const u = String(uid ?? '')
			if (!u) return
			const next = prompt(`Device name for ${u}:`, String(currentName ?? ''))
			if (next === null) return

			setStatus('Renaming...')
			try {
				await postCbor('/api/devices', { device_uid: u, name: String(next) })
				setStatus('Renamed (waiting WS sync)')
			} catch (e) {
				setStatus(String(e?.message ?? e))
			}
		},
		[],
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
					<button onClick={permitJoin}>Scan new devices (permit join)</button>
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
												<button onClick={() => renameDevice(d?.device_uid, d?.name)}>Rename</button>
												<button onClick={() => removeDevice(d?.device_uid)}>Удалить</button>
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
