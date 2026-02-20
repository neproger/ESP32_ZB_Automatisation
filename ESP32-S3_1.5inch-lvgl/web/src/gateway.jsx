//UTF-8
//gateway.jsx
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import { fetchBinary, fetchCbor } from './api.js'
import { cborDecode } from './cbor.js'
import { parseDeviceBlob } from './deviceBlob.js'
import { groupsReload } from './groupsStore.js'

function wsUrl(path) {
	const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
	return `${proto}://${window.location.host}${path}`
}

function normalizeUid(v) {
	return String(v ?? '').trim().toLowerCase()
}

function buildStateMap(items) {
	const out = {}
	;(Array.isArray(items) ? items : []).forEach((it) => {
		const uid = normalizeUid(it?.device_id)
		const epNum = Number(it?.endpoint_id ?? 0)
		const ep = String(Number.isFinite(epNum) && epNum > 0 ? epNum : '')
		const key = String(it?.key ?? '')
		if (!uid || !ep || !key) return
		if (!out[uid]) out[uid] = {}
		if (!out[uid][ep]) out[uid][ep] = {}
		out[uid][ep][key] = it?.value ?? null
	})
	return out
}

const GatewayContext = createContext(null)

export function GatewayProvider({ children }) {
	const [devices, setDevices] = useState([])
	const [automations, setAutomations] = useState([])
	const [events, setEvents] = useState([])
	const [deviceStates, setDeviceStates] = useState({})
	const [projectSettings, setProjectSettings] = useState(null)
	const [wsStatus, setWsStatus] = useState('disconnected')

	const wsRef = useRef(null)
	const reconnectTimerRef = useRef(null)

	const applyDeviceList = useCallback((list) => {
		const safeList = Array.isArray(list) ? list : []
		setDevices(safeList)
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

	const loadStateSnapshot = useCallback(async () => {
		const data = await fetchCbor('/api/state')
		const next = buildStateMap(data?.items)
		setDeviceStates(next)
		return next
	}, [])

	const loadSettings = useCallback(async () => {
		const data = await fetchCbor('/api/settings')
		const next = data?.settings && typeof data.settings === 'object' ? data.settings : null
		setProjectSettings(next)
		return next
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
				if (wsRef.current !== ws) return
				attempts = 0
				setWsStatus('connected')
			}

			ws.onmessage = (ev) => {
				if (wsRef.current !== ws) return
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
						const ep = String(Number.isFinite(epNum) && epNum > 0 ? epNum : '')
						const key = String(data?.key ?? '')
						if (!uid || !ep || !key) return
						setDeviceStates((prev) => ({
							...prev,
							[uid]: {
								...(prev[uid] || {}),
								[ep]: {
									...((prev[uid] && prev[uid][ep]) || {}),
									[key]: data?.value ?? null,
								},
							},
						}))
					}
					if (type === 'gateway.event') {
						const evType = String(data?.event_type ?? '')
						if (evType === 'device.changed') {
							loadDevices().catch(() => {})
							loadStateSnapshot().catch(() => {})
						} else if (evType === 'automation.changed') {
							loadAutomations().catch(() => {})
						} else if (evType === 'group.changed') {
							groupsReload().catch(() => {})
						} else if (evType === 'settings.changed') {
							loadSettings().catch(() => {})
						}
					}
				} catch {
					// ignore parse errors
				}
			}

			ws.onclose = () => {
				if (wsRef.current !== ws) return
				if (cancelled) return
				setWsStatus('disconnected')
				attempts += 1
				const delay = Math.min(5000, 250 * 2 ** Math.min(attempts, 5))
				reconnectTimerRef.current = setTimeout(connect, delay)
			}

			ws.onerror = () => {
				if (wsRef.current !== ws) return
				setWsStatus('error')
			}
		}

		connect()
		return () => {
			cancelled = true
			cleanup()
			setWsStatus('disconnected')
		}
	}, [loadAutomations, loadDevices, loadStateSnapshot, loadSettings])

	useEffect(() => {
		loadDevices().catch(() => {})
		loadAutomations().catch(() => {})
		loadStateSnapshot().catch(() => {})
		loadSettings().catch(() => {})
	}, [loadDevices, loadAutomations, loadStateSnapshot, loadSettings])

	const value = useMemo(
		() => ({
			devices,
			automations,
			events,
			deviceStates,
			projectSettings,
			wsStatus,
			reloadDevices: loadDevices,
			reloadAutomations: loadAutomations,
			reloadSettings: loadSettings,
		}),
		[devices, automations, events, deviceStates, projectSettings, wsStatus, loadDevices, loadAutomations, loadSettings],
	)

	return <GatewayContext.Provider value={value}>{children}</GatewayContext.Provider>
}

export function useGateway() {
	const ctx = useContext(GatewayContext)
	if (!ctx) throw new Error('useGateway must be used inside GatewayProvider')
	return ctx
}
