//UTF-8
//EndpointWidgets.jsx
import { useCallback, useEffect, useMemo, useState } from 'react'
import { execAction } from '../api.js'

function toNumberOrNull(v) {
	const n = Number(v)
	return Number.isFinite(n) ? n : null
}

function toBoolOrNull(v) {
	if (typeof v === 'boolean') return v
	if (typeof v === 'number' && Number.isFinite(v)) return v !== 0
	if (typeof v === 'string') {
		const s = v.trim().toLowerCase()
		if (s === 'true' || s === '1' || s === 'on') return true
		if (s === 'false' || s === '0' || s === 'off') return false
	}
	return null
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
	const h = (n) => n.toString(16).padStart(2, '0')
	return `#${h(r)}${h(g)}${h(b)}`
}

function hasAccept(endpoint, prefix) {
	const arr = Array.isArray(endpoint?.accepts) ? endpoint.accepts : []
	return arr.some((x) => String(x ?? '').startsWith(prefix))
}

function getSensorValue(sensors, endpoint, clusterId, attrId) {
	const ep = Number(endpoint ?? 0)
	const item = (Array.isArray(sensors) ? sensors : []).find(
		(s) =>
			Number(s?.endpoint ?? 0) === ep &&
			Number(s?.cluster_id ?? 0) === Number(clusterId) &&
			Number(s?.attr_id ?? 0) === Number(attrId),
	)
	if (!item) return null
	if (item?.value_u32 != null) return toNumberOrNull(item.value_u32)
	if (item?.value_i32 != null) return toNumberOrNull(item.value_i32)
	return null
}

function deriveState(endpoint, liveStateByEndpoint, sensors) {
	const ep = Number(endpoint?.endpoint ?? 0)
	const st = (liveStateByEndpoint && liveStateByEndpoint[String(ep)]) || {}

	const onoff = typeof st?.onoff === 'boolean'
		? st.onoff
		: (() => {
			const vb = toBoolOrNull(st?.onoff)
			if (vb != null) return vb
			const v = getSensorValue(sensors, ep, 0x0006, 0x0000)
			return v != null ? Number(v) !== 0 : false
		})()

	const level = (() => {
		const v = toNumberOrNull(st?.level)
		if (v != null) return Math.max(0, Math.min(254, Math.round(v)))
		const s = getSensorValue(sensors, ep, 0x0008, 0x0000)
		return s != null ? Math.max(0, Math.min(254, Math.round(s))) : 0
	})()

	const colorX = toNumberOrNull(st?.color_x) ?? getSensorValue(sensors, ep, 0x0300, 0x0003)
	const colorY = toNumberOrNull(st?.color_y) ?? getSensorValue(sensors, ep, 0x0300, 0x0004)
	const colorHex = colorX != null && colorY != null ? xyToRgbHex(colorX, colorY) : '#ffffff'

	const tempK = (() => {
		const mired = toNumberOrNull(st?.color_temp_mireds) ?? getSensorValue(sensors, ep, 0x0300, 0x0007)
		if (mired != null && mired > 0) return Math.max(2000, Math.min(6500, Math.round(1_000_000 / mired)))
		return 3000
	})()

	return {
		onoff,
		level,
		colorHex,
		tempK,
		temperature_c: toNumberOrNull(st?.temperature_c),
		humidity_pct: toNumberOrNull(st?.humidity_pct),
		battery_pct: toNumberOrNull(st?.battery_pct),
	}
}

function readOnlyRows(endpoint, state, sensors) {
	const ep = Number(endpoint?.endpoint ?? 0)
	const rows = []
	const reported = Array.isArray(endpoint?.reports) ? endpoint.reports : []
	const has = (k) => reported.some((x) => String(x) === k)

	if (has('temperature_c')) {
		const v = state.temperature_c ?? getSensorValue(sensors, ep, 0x0402, 0x0000)
		if (v != null) rows.push(`Temperature: ${Number(v).toFixed(1)} °C`)
	}
	if (has('humidity_pct')) {
		const v = state.humidity_pct ?? getSensorValue(sensors, ep, 0x0405, 0x0000)
		if (v != null) rows.push(`Humidity: ${Number(v).toFixed(1)} %`)
	}
	if (has('battery_pct')) {
		const v = state.battery_pct ?? getSensorValue(sensors, ep, 0x0001, 0x0021)
		if (v != null) rows.push(`Battery: ${Math.round(Number(v))}%`)
	}
	return rows
}

function isTempHumidityEndpoint(endpoint) {
	const kind = String(endpoint?.kind ?? '')
	if (kind === 'temp_humidity_sensor') return true
	const reports = Array.isArray(endpoint?.reports) ? endpoint.reports.map((x) => String(x)) : []
	return reports.includes('temperature_c') || reports.includes('humidity_pct')
}

export default function EndpointWidgets({
	endpoint,
	deviceUid,
	liveStateByEndpoint,
	sensors,
	onStatus,
	compact = false,
}) {
	const ep = Number(endpoint?.endpoint ?? 0)
	const state = useMemo(() => deriveState(endpoint, liveStateByEndpoint, sensors), [endpoint, liveStateByEndpoint, sensors])

	const [level, setLevel] = useState(state.level)
	const [color, setColor] = useState(state.colorHex)
	const [tempK, setTempK] = useState(state.tempK)

	useEffect(() => setLevel(state.level), [state.level, ep, deviceUid])
	useEffect(() => setColor(state.colorHex), [state.colorHex, ep, deviceUid])
	useEffect(() => setTempK(state.tempK), [state.tempK, ep, deviceUid])

	const reportStatus = useCallback((msg) => {
		if (typeof onStatus === 'function') onStatus(msg)
	}, [onStatus])

	const sendAction = useCallback(async (payload) => {
		try {
			reportStatus('Sending...')
			await execAction(payload)
			reportStatus('')
		} catch (e) {
			reportStatus(String(e?.message ?? e))
		}
	}, [reportStatus])

	const doOnOff = useCallback((cmd) => {
		return sendAction({ type: 'zigbee', cmd: `onoff.${cmd}`, device_uid: deviceUid, endpoint: ep })
	}, [sendAction, deviceUid, ep])

	const doLevel = useCallback((v) => {
		return sendAction({
			type: 'zigbee',
			cmd: 'level.move_to_level',
			device_uid: deviceUid,
			endpoint: ep,
			level: Math.max(0, Math.min(254, Math.round(Number(v ?? 0)))),
			transition_ms: 300,
		})
	}, [sendAction, deviceUid, ep])

	const doColor = useCallback((hex) => {
		const { x, y } = rgbHexToXy(hex)
		return sendAction({
			type: 'zigbee',
			cmd: 'color.move_to_color_xy',
			device_uid: deviceUid,
			endpoint: ep,
			x,
			y,
			transition_ms: 400,
		})
	}, [sendAction, deviceUid, ep])

	const doTemp = useCallback((kelvin) => {
		const k = Math.max(2000, Math.min(6500, Math.round(Number(kelvin ?? 3000))))
		return sendAction({
			type: 'zigbee',
			cmd: 'color.move_to_color_temperature',
			device_uid: deviceUid,
			endpoint: ep,
			mireds: Math.round(1_000_000 / k),
			transition_ms: 600,
		})
	}, [sendAction, deviceUid, ep])

	const rows = readOnlyRows(endpoint, state, sensors)
	const showSensorCard = isTempHumidityEndpoint(endpoint)
	const sensorTemp = (() => {
		const ep = Number(endpoint?.endpoint ?? 0)
		return state.temperature_c ?? getSensorValue(sensors, ep, 0x0402, 0x0000)
	})()
	const sensorHum = (() => {
		const ep = Number(endpoint?.endpoint ?? 0)
		return state.humidity_pct ?? getSensorValue(sensors, ep, 0x0405, 0x0000)
	})()

	return (
		<div className={`endpoint-widgets${compact ? ' compact' : ''}`}>
			{hasAccept(endpoint, 'onoff.') ? (
				<div className="endpoint-control-row">
					<label className="muted" style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
						<input
							type="checkbox"
							checked={Boolean(state.onoff)}
							onChange={(ev) => doOnOff(ev?.target?.checked ? 'on' : 'off')}
						/>
						On
					</label>
					<button onClick={() => doOnOff('toggle')}>Toggle</button>
				</div>
			) : null}

			{hasAccept(endpoint, 'level.') ? (
				<div className="endpoint-control-col">
					<div className="muted">Level</div>
					<input
						type="range"
						min={0}
						max={254}
						value={level}
						onChange={(ev) => {
							const v = Number(ev?.target?.value ?? 0)
							setLevel(v)
							doLevel(v)
						}}
					/>
				</div>
			) : null}

			{hasAccept(endpoint, 'color.move_to_color_xy') ? (
				<div className="endpoint-control-col">
					<div className="muted">Color</div>
					<input
						type="color"
						value={color}
						onChange={(ev) => {
							const v = String(ev?.target?.value ?? '#ffffff')
							setColor(v)
							doColor(v)
						}}
					/>
				</div>
			) : null}

			{hasAccept(endpoint, 'color.move_to_color_temperature') ? (
				<div className="endpoint-control-col">
					<div className="muted">Color Temperature (K)</div>
					<input
						type="range"
						min={2000}
						max={6500}
						step={100}
						value={tempK}
						onChange={(ev) => {
							const v = Number(ev?.target?.value ?? 3000)
							setTempK(v)
							doTemp(v)
						}}
					/>
				</div>
			) : null}

			{showSensorCard ? (
				<div className="sensor-state-grid">
					<div className="sensor-state-card">
						<div className="sensor-state-label">Temperature</div>
						<div className="sensor-state-value">
							{sensorTemp != null && Number.isFinite(Number(sensorTemp))
								? `${Number(sensorTemp).toFixed(1)} °C`
								: '-'}
						</div>
					</div>
					<div className="sensor-state-card">
						<div className="sensor-state-label">Humidity</div>
						<div className="sensor-state-value">
							{sensorHum != null && Number.isFinite(Number(sensorHum))
								? `${Number(sensorHum).toFixed(1)} %`
								: '-'}
						</div>
					</div>
				</div>
			) : rows.map((line) => (
				<div key={line} className="muted">{line}</div>
			))}
		</div>
	)
}
