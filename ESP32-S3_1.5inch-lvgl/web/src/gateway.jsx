//UTF-8
//gateway.jsx
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import { groupsProtoApplyFrame } from './groupsStore.js'
import { setWsCommandSender } from './wsCommandBus.js'
import {
	protoApplyFrame,
	protoEncodeActionExec,
	protoEncodeAutomationChange,
	protoEncodeAutomationRemove,
	protoEncodeAutomationResetAll,
	protoEncodeDeviceChange,
	protoCreateSnapshotAccumulator,
	protoEncodeDeviceRemove,
	protoEncodeDeviceRemoveAll,
	protoEncodeFactoryReset,
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

function protoTypeName(type) {
	const names = {
		0x40: 'SYNC_BEGIN',
		0x41: 'SYNC_END',
		0x42: 'DEVICE_UPSERT',
		0x43: 'DEVICE_REMOVE',
		0x44: 'ENDPOINT_UPSERT',
		0x45: 'ENDPOINT_REMOVE',
		0x46: 'STATE_ITEM',
		0x47: 'STATE_REMOVE',
		0x48: 'GROUP_UPSERT',
		0x49: 'GROUP_REMOVE',
		0x4a: 'GROUP_ITEM_UPSERT',
		0x4b: 'GROUP_ITEM_REMOVE',
		0x4c: 'SETTINGS',
		0x4d: 'SNAPSHOT_REQUEST',
		0x4e: 'AUTOMATION_UPSERT',
		0x4f: 'AUTOMATION_REMOVE',
		0x50: 'CMD_PERMIT_JOIN',
		0x52: 'CMD_DEVICE_REMOVE',
		0x53: 'CMD_DEVICE_REMOVE_ALL',
		0x54: 'CMD_GROUP_CREATE',
		0x56: 'CMD_GROUP_DELETE',
		0x5d: 'CMD_AUTOMATION_REMOVE',
		0x5e: 'CMD_AUTOMATION_RESET_ALL',
		0x60: 'CMD_ACTION_EXEC',
		0x70: 'EVENT_TRACE',
		0x72: 'CMD_FACTORY_RESET',
		0x73: 'CMD_DEVICE_CHANGE',
		0x74: 'CMD_GROUP_CHANGE',
		0x75: 'CMD_AUTOMATION_CHANGE',
		0x76: 'CMD_SETTINGS_CHANGE',
		0x77: 'CMD_GROUP_ITEMS_CHANGE',
	}
	return names[type] ?? `0x${Number(type ?? 0).toString(16).padStart(2, '0')}`
}

function logProtoFrame(direction, frame) {
	if (!frame) return
	console.debug(`[ws ${direction}] ${protoTypeName(frame.type)}`, {
		type: frame.type,
		seq: frame.seq,
		len: frame.len,
	})
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
		logProtoFrame('tx', protoTryParseFrame(buffer))
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
		const frame = protoEncodeSnapshotRequest(Date.now() & 0xffff)
		logProtoFrame('tx', protoTryParseFrame(frame))
		ws.send(frame)
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
				const frame = protoEncodeSnapshotRequest(Date.now() & 0xffff)
				logProtoFrame('tx', protoTryParseFrame(frame))
				ws.send(frame)
			}

			ws.onmessage = (ev) => {
				if (wsRef.current !== ws) return
				try {
					if (!(ev?.data instanceof ArrayBuffer)) return
					const protoFrame = protoTryParseFrame(ev.data)
					if (protoFrame) {
						logProtoFrame('rx', protoFrame)
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
						protoApplyFrame(protoSnapshotRef.current, protoFrame, (data) => {
							if (Array.isArray(data?.devices)) {
								applyDeviceList(data.devices)
							}
							if (data?.deviceStates && typeof data.deviceStates === 'object') {
								setDeviceStates(data.deviceStates)
							}
							if (Array.isArray(data?.automations)) {
								setAutomations(data.automations)
							}
							if (Array.isArray(data?.groups)) {
								setGroups(data.groups)
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
		const frame = protoEncodeSnapshotRequest(Date.now() & 0xffff)
		logProtoFrame('tx', protoTryParseFrame(frame))
		ws.send(frame)
		return automations
	}, [automations])

	const permitJoin = useCallback(async (seconds = 180) => {
		sendProtoCommand(protoEncodePermitJoin(seconds, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const renameDevice = useCallback(async (deviceUid, name) => {
		sendProtoCommand(protoEncodeDeviceChange({ device_uid: deviceUid, name }, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const removeDevice = useCallback(async (deviceUid) => {
		sendProtoCommand(protoEncodeDeviceRemove(deviceUid, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const removeAllDevices = useCallback(async () => {
		sendProtoCommand(protoEncodeDeviceRemoveAll(nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const factoryReset = useCallback(async () => {
		sendProtoCommand(protoEncodeFactoryReset(nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const setAutomationEnabled = useCallback(async (id, enabled) => {
		const current = Array.isArray(automations) ? automations.find((it) => String(it?.id ?? '') === String(id ?? '')) : null
		if (!current) {
			throw new Error(`automation not found: ${String(id ?? '')}`)
		}
		sendProtoCommand(protoEncodeAutomationChange({ ...current, enabled: Boolean(enabled) }, nextSeq()))
	}, [automations, nextSeq, sendProtoCommand])

	const removeAutomation = useCallback(async (id) => {
		sendProtoCommand(protoEncodeAutomationRemove(id, nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const resetAllAutomations = useCallback(async () => {
		sendProtoCommand(protoEncodeAutomationResetAll(nextSeq()))
	}, [nextSeq, sendProtoCommand])

	const saveAutomation = useCallback(async (draft) => {
		sendProtoCommand(protoEncodeAutomationChange(draft, nextSeq()))
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
			factoryReset,
			setAutomationEnabled,
			removeAutomation,
			resetAllAutomations,
			saveAutomation,
			execActions,
		}),
		[devices, automations, events, deviceStates, projectSettings, wsStatus, loadDevices, loadAutomations, permitJoin, renameDevice, removeDevice, removeAllDevices, factoryReset, setAutomationEnabled, removeAutomation, resetAllAutomations, saveAutomation, execActions],
	)

	return <GatewayContext.Provider value={value}>{children}</GatewayContext.Provider>
}

export function useGateway() {
	const ctx = useContext(GatewayContext)
	if (!ctx) throw new Error('useGateway must be used inside GatewayProvider')
	return ctx
}
