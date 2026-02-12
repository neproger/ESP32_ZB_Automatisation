const KEY_FORMATTERS = {
	temperature_c: (value) => {
		const n = Number(value)
		return Number.isFinite(n) ? `${n.toFixed(1)} \u00B0C` : String(value ?? '')
	},
	humidity_pct: (value) => {
		const n = Number(value)
		return Number.isFinite(n) ? `${n.toFixed(1)} %` : String(value ?? '')
	},
	onoff: (value) => (Boolean(value) ? 'On' : 'Off'),
	level: (value) => {
		const n = Number(value)
		if (!Number.isFinite(n)) return String(value ?? '')
		const pct = Math.max(0, Math.min(100, Math.round((n / 254) * 100)))
		return `${pct}% (${Math.round(n)}/254)`
	},
	battery_pct: (value) => {
		const n = Number(value)
		return Number.isFinite(n) ? `${Math.round(n)}%` : String(value ?? '')
	},
	battery_mv: (value) => {
		const n = Number(value)
		return Number.isFinite(n) ? `${Math.round(n)} mV` : String(value ?? '')
	},
	color_temp_mireds: (value) => {
		const n = Number(value)
		return Number.isFinite(n) ? `${Math.round(n)} mired` : String(value ?? '')
	},
	color_x: (value) => {
		const n = Number(value)
		return Number.isFinite(n) ? `${Math.round(n)}` : String(value ?? '')
	},
	color_y: (value) => {
		const n = Number(value)
		return Number.isFinite(n) ? `${Math.round(n)}` : String(value ?? '')
	},
}

const KEY_LABELS = {
	temperature_c: 'Температура',
	humidity_pct: 'Влажность',
	onoff: 'Состояние',
	level: 'Уровень',
	battery_pct: 'Батарея',
	battery_mv: 'Напряжение батареи',
	color_temp_mireds: 'Цветовая температура',
	color_x: 'Цвет X',
	color_y: 'Цвет Y',
}

export function getDeviceStateKeyLabel(key) {
	const k = String(key ?? '')
	return KEY_LABELS[k] || k
}

export function formatDeviceStateValue(key, value) {
	const fn = KEY_FORMATTERS[String(key ?? '')]
	return fn ? fn(value) : String(value ?? '')
}

export function formatDeviceStateLine(data) {
	const key = String(data?.key ?? '')
	const endpoint = Number(data?.endpoint_id ?? 0)
	const label = getDeviceStateKeyLabel(key)
	const pretty = formatDeviceStateValue(key, data?.value)
	return `ep${endpoint} ${label}: ${pretty}`
}
