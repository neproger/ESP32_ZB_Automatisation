import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import { fetchBinary, fetchCbor } from './api.js'
import { cborDecode } from './cbor.js'
import { parseDeviceBlob } from './deviceBlob.js'

function wsUrl(path) {
	const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
	return `${proto}://${window.location.host}${path}`
}

function normalizeUid(v) {
	return String(v ?? '').trim().toLowerCase()
}

function normalizeEndpointStateFromSnapshot(stateObj = {}) {
	const endpointState = {}
	for (const [k, v] of Object.entries(stateObj || {})) {
		if (!endpointState['0']) endpointState['0'] = {}
		endpointState['0'][k] = v
	}
	return endpointState
}

function sensorToState(sensor) {
	const cluster = Number(sensor?.cluster_id ?? 0)
	const attr = Number(sensor?.attr_id ?? 0)
	if (cluster === 0x0006 && attr === 0x0000) {
		const raw = Number(sensor?.value_u32 ?? sensor?.value_i32 ?? 0)
		return { key: 'onoff', value: raw !== 0 }
	}
	if (cluster === 0x0402 && attr === 0x0000) {
		const raw = Number(sensor?.value_i32 ?? 0)
		return { key: 'temperature_c', value: raw / 100.0 }
	}
	if (cluster === 0x0405 && attr === 0x0000) {
		const raw = Number(sensor?.value_u32 ?? 0)
		return { key: 'humidity_pct', value: raw / 100.0 }
	}
	if (cluster === 0x0001 && attr === 0x0021) {
		const raw = Number(sensor?.value_u32 ?? 0)
		return { key: 'battery_pct', value: Math.floor(raw / 2) }
	}
	return null
}

const GatewayContext = createContext(null)

export function GatewayProvider({ children }) {
	const [devices, setDevices] = useState([])
	const [automations, setAutomations] = useState([])
	const [events, setEvents] = useState([])
	const [deviceStates, setDeviceStates] = useState({})
	const [wsStatus, setWsStatus] = useState('disconnected')

	const wsRef = useRef(null)
	const reconnectTimerRef = useRef(null)

	const applyDeviceList = useCallback((list) => {
		const safeList = Array.isArray(list) ? list : []
		setDevices(safeList)
		setDeviceStates((prev) => {
			const next = { ...prev }
			for (const d of safeList) {
				const uid = String(d?.device_uid ?? '')
				if (!uid) continue
				const fromSnapshot = normalizeEndpointStateFromSnapshot(d?.state && typeof d.state === 'object' ? d.state : {})
				if (!next[uid]) next[uid] = {}
				for (const [ep, st] of Object.entries(fromSnapshot)) {
					next[uid][ep] = { ...(next[uid][ep] || {}), ...(st || {}) }
				}
				const sensors = Array.isArray(d?.sensors) ? d.sensors : []
				for (const s of sensors) {
					const endpoint = String(Number(s?.endpoint ?? 0))
					if (!endpoint || endpoint === '0') continue
					const mapped = sensorToState(s)
					if (!mapped) continue
					if (!next[uid][endpoint]) next[uid][endpoint] = {}
					next[uid][endpoint][mapped.key] = mapped.value
				}
			}
			return next
		})
		return safeList
	}, [])

	const loadDevices = useCallback(async () => {
		const blob = await fetchBinary('/api/devices/flatbuffer')
		const list = parseDeviceBlob(blob)
		return applyDeviceList(list)
	}, [applyDeviceList])

	const loadAutomations = useCallback(async () => {
		const data = await fetchCbor('/api/automations')
		const list = Array.isArray(data?.automations) ? data.automations : []
		setAutomations(list)
		return list
	}, [])

	useEffect(() => {
		let cancelled = false
		let attempts = 0

		const cleanup = () => {
			if (reconnectTimerRef.current) {
				clearTimeout(reconnectTimerRef.current)
				reconnectTimerRef.current = null
			}
			if (wsRef.current) {
				try { wsRef.current.close() } catch {}
				wsRef.current = null
			}
		}

		const connect = () => {
			cleanup()
			setWsStatus('connecting')
			const ws = new WebSocket(wsUrl('/ws'))
			ws.binaryType = 'arraybuffer'
			wsRef.current = ws

			ws.onopen = () => {
				attempts = 0
				setWsStatus('connected')
			}

			ws.onmessage = (ev) => {
				try {
					if (!(ev?.data instanceof ArrayBuffer)) return
					const msg = cborDecode(ev.data)
					if (!msg || typeof msg !== 'object') return
					const type = String(msg?.type ?? '')
					const data = msg?.data && typeof msg.data === 'object' ? msg.data : {}
					if (!type) return

					setEvents((prev) => {
						const next = [...prev, msg]
						return next.length > 30 ? next.slice(next.length - 30) : next
					})

					if (type === 'device.state') {
						const uid = normalizeUid(data?.device_id ?? '')
						const epNum = Number(data?.endpoint_id ?? data?.endpoint ?? 0)
						const ep = String(Number.isFinite(epNum) && epNum > 0 ? epNum : 0)
						const key = String(data?.key ?? '')
						if (!uid || !ep || !key) return
						setDeviceStates((prev) => ({
							...prev,
							[uid]: {
								...(prev[uid] || {}),
								0: {
									...((prev[uid] && prev[uid]['0']) || {}),
									[key]: data?.value ?? null,
								},
								[ep]: {
									...((prev[uid] && prev[uid][ep]) || {}),
									[key]: data?.value ?? null,
								},
							},
						}))
					}
				} catch {
					// ignore parse errors
				}
			}

			ws.onclose = () => {
				if (cancelled) return
				setWsStatus('disconnected')
				attempts += 1
				const delay = Math.min(5000, 250 * 2 ** Math.min(attempts, 5))
				reconnectTimerRef.current = setTimeout(connect, delay)
			}

			ws.onerror = () => {
				setWsStatus('error')
			}
		}

		connect()
		return () => {
			cancelled = true
			cleanup()
			setWsStatus('disconnected')
		}
	}, [])

	useEffect(() => {
		loadAutomations().catch(() => {})
	}, [loadDevices, loadAutomations])

	useEffect(() => {
		let cancelled = false
		let timer = null
		let attempt = 0

		const schedule = (ms) => {
			if (cancelled) return
			if (timer) clearTimeout(timer)
			timer = setTimeout(run, ms)
		}

		const run = async () => {
			try {
				const list = await loadDevices()
				if (cancelled) return
				const count = Array.isArray(list) ? list.length : 0
				// Device snapshot can be unavailable right after boot; retry while empty.
				if (count === 0 && attempt < 20) {
					attempt += 1
					schedule(Math.min(5000, 250 * 2 ** Math.min(attempt, 5)))
				}
			} catch {
				if (cancelled) return
				if (attempt < 20) {
					attempt += 1
					schedule(Math.min(5000, 250 * 2 ** Math.min(attempt, 5)))
				}
			}
		}

		run()
		return () => {
			cancelled = true
			if (timer) clearTimeout(timer)
		}
	}, [loadDevices])

	const value = useMemo(
		() => ({
			devices,
			automations,
			events,
			deviceStates,
			wsStatus,
			reloadDevices: loadDevices,
			reloadAutomations: loadAutomations,
		}),
		[devices, automations, events, deviceStates, wsStatus, loadDevices, loadAutomations],
	)

	return <GatewayContext.Provider value={value}>{children}</GatewayContext.Provider>
}

export function useGateway() {
	const ctx = useContext(GatewayContext)
	if (!ctx) throw new Error('useGateway must be used inside GatewayProvider')
	return ctx
}
