//UTF-8
//AutomationEditor.jsx
import { useCallback, useMemo } from 'react'
import { ensureArray, getDeviceOptions } from './utils.js'
import TriggersSection from './sections/TriggersSection.jsx'
import ConditionsSection from './sections/ConditionsSection.jsx'
import ActionsSection from './sections/ActionsSection.jsx'

export default function AutomationEditor({
	draft,
	setDraft,
	status,
	onSave,
	onTestActions,
	devices,
	endpointsByUid,
	ensureEndpoints,
}) {
	const triggers = ensureArray(draft?.triggers)
	const conditions = ensureArray(draft?.conditions)
	const actions = ensureArray(draft?.actions)
	const deviceOptions = useMemo(() => getDeviceOptions(devices), [devices])

	const setField = (k, v) => setDraft((cur) => ({ ...(cur ?? {}), [k]: v }))

	const updateTrigger = (idx, patch) => {
		setField('triggers', triggers.map((t, i) => (i === idx ? { ...(t ?? {}), ...(patch ?? {}) } : t)))
	}
	const updateCondition = (idx, patch) => {
		setField('conditions', conditions.map((c, i) => (i === idx ? { ...(c ?? {}), ...(patch ?? {}) } : c)))
	}
	const updateAction = (idx, patch) => {
		setField('actions', actions.map((a, i) => (i === idx ? { ...(a ?? {}), ...(patch ?? {}) } : a)))
	}

	const removeTrigger = (idx) => setField('triggers', triggers.filter((_, i) => i !== idx))
	const removeCondition = (idx) => setField('conditions', conditions.filter((_, i) => i !== idx))
	const removeAction = (idx) => setField('actions', actions.filter((_, i) => i !== idx))

	const addTrigger = () => setField('triggers', [...triggers, { type: 'event', event_type: 'zigbee.command', match: { 'payload.cmd': 'toggle' } }])
	const addCondition = () => {
		const fromTrigger = String(triggers?.[0]?.match?.device_uid ?? '')
		setField('conditions', [...conditions, { type: 'state', op: '>', ref: { device_uid: fromTrigger, endpoint: 1, key: 'temperature_c' }, value: 10 }])
	}
	const addAction = () => setField('actions', [...actions, { type: 'zigbee', cmd: 'onoff.toggle', device_uid: '', endpoint: 1 }])

	const devicesByUid = useMemo(() => {
		const m = new Map()
		for (const d of Array.isArray(devices) ? devices : []) {
			const uid = String(d?.device_uid ?? '')
			if (uid) m.set(uid, d)
		}
		return m
	}, [devices])

	const reportToClusterAttr = useMemo(() => ({
		temperature_c: { cluster: '0x0402', attr: '0x0000' },
		humidity_pct: { cluster: '0x0405', attr: '0x0000' },
		battery_pct: { cluster: '0x0001', attr: '0x0021' },
		onoff: { cluster: '0x0006', attr: '0x0000' },
		level: { cluster: '0x0008', attr: '0x0000' },
		occupancy: { cluster: '0x0406', attr: '0x0000' },
		illuminance: { cluster: '0x0400', attr: '0x0000' },
	}), [])

	const clusterAttrToReportKey = useCallback((cluster, attr) => {
		const c = String(cluster ?? '')
		const a = String(attr ?? '')
		if (!c || !a) return ''
		for (const [k, v] of Object.entries(reportToClusterAttr)) {
			if (v?.cluster === c && v?.attr === a) return k
		}
		return ''
	}, [reportToClusterAttr])

	const jsonPreview = useMemo(() => {
		try {
			return JSON.stringify(draft, null, 2)
		} catch {
			return ''
		}
	}, [draft])

	return (
		<div className="card" style={{ padding: 12 }}>
			<div className="row" style={{ justifyContent: 'space-between' }}>
				<div>
					<div className="muted">Automation definition</div>
					<div className="row" style={{ marginTop: 6 }}>
						<label className="muted">id</label>
						<input value={String(draft?.id ?? '')} onChange={(e) => setField('id', String(e.target.value ?? ''))} placeholder="auto_1" style={{ minWidth: 220 }} />
						<label className="muted" style={{ marginLeft: 10 }}>name</label>
						<input value={String(draft?.name ?? '')} onChange={(e) => setField('name', String(e.target.value ?? ''))} placeholder="My automation" style={{ minWidth: 260 }} />
						<label className="row" style={{ gap: 6, marginLeft: 10 }}>
							<input type="checkbox" checked={Boolean(draft?.enabled)} onChange={(e) => setField('enabled', Boolean(e.target.checked))} />
							<span className="muted">enabled</span>
						</label>
					</div>
				</div>
				<div className="row">
					<button onClick={onTestActions} disabled={actions.length === 0}>Test actions</button>
					<button onClick={onSave}>Save</button>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div style={{ height: 10 }} />

			<div className="row" style={{ alignItems: 'flex-start' }}>
				<TriggersSection
					triggers={triggers}
					deviceOptions={deviceOptions}
					devicesByUid={devicesByUid}
					endpointsByUid={endpointsByUid}
					ensureEndpoints={ensureEndpoints}
					updateTrigger={updateTrigger}
					removeTrigger={removeTrigger}
					addTrigger={addTrigger}
					reportToClusterAttr={reportToClusterAttr}
					clusterAttrToReportKey={clusterAttrToReportKey}
				/>

				<ConditionsSection
					conditions={conditions}
					deviceOptions={deviceOptions}
					endpointsByUid={endpointsByUid}
					ensureEndpoints={ensureEndpoints}
					updateCondition={updateCondition}
					removeCondition={removeCondition}
					addCondition={addCondition}
				/>

				<ActionsSection
					actions={actions}
					deviceOptions={deviceOptions}
					devicesByUid={devicesByUid}
					endpointsByUid={endpointsByUid}
					ensureEndpoints={ensureEndpoints}
					updateAction={updateAction}
					removeAction={removeAction}
					addAction={addAction}
				/>
			</div>

			<div style={{ height: 10 }} />

			<details>
				<summary className="muted">Raw JSON</summary>
				<textarea value={jsonPreview} readOnly className="mono" style={{ width: '100%', minHeight: 220, marginTop: 8, padding: 10, borderRadius: 10 }} />
			</details>
		</div>
	)
}
