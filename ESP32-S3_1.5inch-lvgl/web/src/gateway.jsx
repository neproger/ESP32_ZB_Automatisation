//UTF-8
//gateway.jsx
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import { groupsProtoApplyFrame } from './groupsStore.js'
import { setWsCommandSender } from './wsCommandBus.js'
import {
	protoApplyFrame,
	protoEncodeActionExec,
	protoEncodeAutomationSave,
	protoEncodeAutomationRemove,
	protoEncodeAutomationResetAll,
	protoEncodeAutomationSetEnabled,
	protoCreateSnapshotAccumulator,
	protoEncodeDeviceRemove,
	protoEncodeDeviceRemoveAll,
	protoEncodeDeviceRename,
	protoEncodePermitJoin,
	protoEncodeSnapshotRequest,
	protoFrameToEvent,
	protoParseSettingsFrame,
	protoTryParseFrame,
} from './proto.js'

function wsUrl(path) {
	const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
	return `${proto}://${window.location.host}${path}`
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
	const seqRef = useRef(1)

	const nextSeq = useCallback(() => {
		const v = seqRef.current & 0xffff
		seqRef.current = ((seqRef.current + 1) & 0xffff) || 1
		return v
	}, [])

	const sendProtoCommand = useCallback((buffer) => {
		const ws = wsRef.current
		if (!ws || ws.readyState !== WebSocket.OPEN) {
			throw new Error('ws not connected')
		}
		ws.send(buffer)
	}, [])

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
				setWsCommandSender(sendProtoCommand)
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
						protoApplyFrame(protoSnapshotRef.current, protoFrame, ({
							devices: nextDevices,
							deviceStates: nextStates,
							automations: nextAutomations,
						}) => {
							if (Array.isArray(nextDevices)) {
								applyDeviceList(nextDevices)
							}
							if (nextStates && typeof nextStates === 'object') {
								setDeviceStates(nextStates)
							}
							if (Array.isArray(nextAutomations)) {
								setAutomations(nextAutomations)
							}
						})
						return
					}
				} catch {
					// ignore parse errors
				}
			}

			ws.onclose = () => {
				if (wsRef.current !== ws) return
				if (cancelled) return
				setWsCommandSender(null)
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
			setWsCommandSender(null)
			cleanup()
			setWsStatus('disconnected')
		}
	}, [applyDeviceList, sendProtoCommand])

	const loadAutomations = useCallback(async () => {
		const ws = wsRef.current
		if (!ws || ws.readyState !== WebSocket.OPEN) {
			throw new Error('ws not connected')
		}
		protoSnapshotRef.current = protoCreateSnapshotAccumulator()
		ws.send(protoEncodeSnapshotRequest(Date.now() & 0xffff))
		return automations
	}, [automations])

	const permitJoin = useCallback(async (seconds = 180) => {
		sendProtoCommand(protoEncodePermitJoin(seconds, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const renameDevice = useCallback(async (deviceUid, name) => {
		sendProtoCommand(protoEncodeDeviceRename(deviceUid, name, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const removeDevice = useCallback(async (deviceUid) => {
		sendProtoCommand(protoEncodeDeviceRemove(deviceUid, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const removeAllDevices = useCallback(async () => {
		sendProtoCommand(protoEncodeDeviceRemoveAll(nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const setAutomationEnabled = useCallback(async (id, enabled) => {
		sendProtoCommand(protoEncodeAutomationSetEnabled(id, enabled, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const removeAutomation = useCallback(async (id) => {
		sendProtoCommand(protoEncodeAutomationRemove(id, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const resetAllAutomations = useCallback(async () => {
		sendProtoCommand(protoEncodeAutomationResetAll(nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const saveAutomation = useCallback(async (draft) => {
		sendProtoCommand(protoEncodeAutomationSave(draft, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const execActions = useCallback(async (actions) => {
		sendProtoCommand(protoEncodeActionExec(actions, nextSeq()))
	}, [nextSeq, sendProtoCommand])

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
			permitJoin,
			renameDevice,
			removeDevice,
			removeAllDevices,
			setAutomationEnabled,
			removeAutomation,
			resetAllAutomations,
			saveAutomation,
			execActions,
		}),
		[devices, automations, events, deviceStates, projectSettings, wsStatus, loadDevices, loadAutomations, permitJoin, renameDevice, removeDevice, removeAllDevices, setAutomationEnabled, removeAutomation, resetAllAutomations, saveAutomation, execActions],
	)

	return <GatewayContext.Provider value={value}>{children}</GatewayContext.Provider>
}

export function useGateway() {
	const ctx = useContext(GatewayContext)
	if (!ctx) throw new Error('useGateway must be used inside GatewayProvider')
	return ctx
}
