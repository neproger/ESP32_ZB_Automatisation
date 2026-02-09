import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { useGateway } from '../gateway.jsx'

function fmtTs(tsMs) {
	const v = Number(tsMs ?? 0)
	if (!Number.isFinite(v) || v <= 0) return ''
	return new Date(v).toLocaleTimeString()
}

export default function Events() {
	const { events, wsStatus } = useGateway()
	const [paused, setPaused] = useState(false)
	const [clearedAtMs, setClearedAtMs] = useState(0)
	const [status] = useState('')
	const scrollRef = useRef(null)
	const pinnedToBottomRef = useRef(true)

	const isNearBottom = (el) => {
		if (!el) return false
		const gap = el.scrollHeight - el.scrollTop - el.clientHeight
		return gap <= 48
	}

	useEffect(() => {
		const el = scrollRef.current
		if (!el) return
		const onScroll = () => {
			pinnedToBottomRef.current = isNearBottom(el)
		}
		el.addEventListener('scroll', onScroll, { passive: true })
		onScroll()
		return () => el.removeEventListener('scroll', onScroll)
	}, [])

	useLayoutEffect(() => {
		if (paused) return
		const el = scrollRef.current
		if (!el) return
		if (!pinnedToBottomRef.current) return
		el.scrollTop = el.scrollHeight
	}, [events, paused])

	const sortedEvents = useMemo(() => {
		let base = paused ? [] : events
		if (clearedAtMs > 0) {
			base = base.filter((e) => Number(e?.ts_ms ?? 0) >= clearedAtMs)
		}
		return [...base].sort((a, b) => Number(a?.ts_ms ?? 0) - Number(b?.ts_ms ?? 0))
	}, [events, paused, clearedAtMs])

	const clear = useCallback(() => {
		setClearedAtMs(Date.now())
	}, [])

	const payloadToText = useCallback((payload) => {
		if (payload == null) return ''
		try {
			return JSON.stringify(payload, null, 2)
		} catch {
			return String(payload)
		}
	}, [])

	const dataDeviceLabel = useCallback((e) => {
		const uid = String(e?.data?.device_id ?? '')
		return uid || '-'
	}, [])

	return (
		<div className="page">
			<div className="header">
				<div>
					<h1>Events</h1>
					<div className="muted">WebSocket stream (/ws), CBOR envelope: ts_ms/type/data.</div>
				</div>
				<div className="row">
					<button onClick={() => setPaused((p) => !p)}>{paused ? 'Resume' : 'Pause'}</button>
					<button onClick={clear}>Clear</button>
					<div className="muted">ws: {wsStatus}</div>
					<div className="muted">count: {sortedEvents.length}</div>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div ref={scrollRef} className="card scroll height-100">
				<div className="table-wrap">
					<table>
						<thead>
							<tr>
								<th>Time</th>
								<th>Type</th>
								<th>Device</th>
								<th>Data</th>
							</tr>
						</thead>
						<tbody>
							{sortedEvents.length === 0 ? (
								<tr>
									<td colSpan={4} className="muted">
										No events yet.
									</td>
								</tr>
							) : (
								sortedEvents.map((e, index) => (
									<tr key={`${Number(e?.ts_ms ?? 0)}_${index}`}>
										<td className="muted">
											{fmtTs(e?.ts_ms)}
										</td>
										<td>{String(e?.type ?? '')}</td>
										<td>{dataDeviceLabel(e)}</td>
										<td className="mono" style={{ whiteSpace: 'pre-wrap' }}>
											{payloadToText(e?.data)}
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
