//UTF-8
//gateway.jsx
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import { fetchCbor } from './api.js'
import { cborDecode } from './cbor.js'
import { groupsProtoApplyFrame } from './groupsStore.js'
import {
	protoApplyFrame,
	protoCreateSnapshotAccumulator,
	protoEncodeSnapshotRequest,
	protoFrameToEvent,
	protoParseSettingsFrame,
	protoTryParseFrame,
} from './proto.js'

function wsUrl(path) {
	const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
	return `${proto}://${window.location.host}${path}`
}

function normalizeUid(v) {
	return String(v ?? '').trim().toLowerCase()
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
	const protoSnapshotRef = useRef(protoCreateSnapshotAccumulator())

	const applyDeviceList = useCallback((list) => {
		const safeList = Array.isArray(list) ? list : []
		setDevices(safeList)
		return safeList
	}, [])

	const loadDevices = useCallback(async () => {
		const ws = wsRef.current
		if (!ws || ws.readyState !== WebSocket.OPEN) {
			throw new Error('ws not connected')
		}
		protoSnapshotRef.current = protoCreateSnapshotAccumulator()
		ws.send(protoEncodeSnapshotRequest(Date.now() & 0xffff))
		return devices
	}, [devices])

	const loadAutomations = useCallback(async () => {
		const data = await fetchCbor('/api/automations')
		const list = Array.isArray(data?.automations) ? data.automations : []
		setAutomations(list)
		return list
	}, [])

	const loadSettings = useCallback(async () => {
		const ws = wsRef.current
		if (!ws || ws.readyState !== WebSocket.OPEN) {
			throw new Error('ws not connected')
		}
		protoSnapshotRef.current = protoCreateSnapshotAccumulator()
		ws.send(protoEncodeSnapshotRequest(Date.now() & 0xffff))
		return projectSettings
	}, [projectSettings])

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
				protoSnapshotRef.current = protoCreateSnapshotAccumulator()
				ws.send(protoEncodeSnapshotRequest(Date.now() & 0xffff))
			}

			ws.onmessage = (ev) => {
				if (wsRef.current !== ws) return
				try {
					if (!(ev?.data instanceof ArrayBuffer)) return
					const protoFrame = protoTryParseFrame(ev.data)
					if (protoFrame) {
						const protoSettings = protoParseSettingsFrame(protoFrame)
						if (protoSettings) {
							setProjectSettings(protoSettings)
						}
						const protoEvent = protoFrameToEvent(protoFrame)
						groupsProtoApplyFrame(protoFrame)
						if (protoEvent) {
							setEvents((prev) => {
								const next = [...prev, protoEvent]
								return next.length > 30 ? next.slice(next.length - 30) : next
							})
						}
						protoApplyFrame(protoSnapshotRef.current, protoFrame, ({ devices: nextDevices, deviceStates: nextStates }) => {
							applyDeviceList(nextDevices)
							setDeviceStates(nextStates)
						})
						return
					}
					const msg = cborDecode(ev.data)
					if (!msg || typeof msg !== 'object') return
					const type = String(msg?.type ?? '')
					const data = msg?.data && typeof msg.data === 'object' ? msg.data : {}
					if (!type) return

					setEvents((prev) => {
						const next = [...prev, msg]
						return next.length > 30 ? next.slice(next.length - 30) : next
					})

					if (type === 'gateway.event') {
						const evType = String(data?.event_type ?? '')
						if (evType === 'automation.changed') {
							loadAutomations().catch(() => {})
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
	}, [applyDeviceList, loadAutomations])

	useEffect(() => {
		loadAutomations().catch(() => {})
	}, [loadAutomations])

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
		}),
		[devices, automations, events, deviceStates, projectSettings, wsStatus, loadDevices, loadAutomations],
	)

	return <GatewayContext.Provider value={value}>{children}</GatewayContext.Provider>
}

export function useGateway() {
	const ctx = useContext(GatewayContext)
	if (!ctx) throw new Error('useGateway must be used inside GatewayProvider')
	return ctx
}
