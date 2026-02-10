import { useCallback, useMemo, useState } from 'react'
import { postCbor, patchCbor, deleteCbor, execAction } from '../api.js'
import { useGateway } from '../gateway.jsx'

function defaultAutomationDef({ id, name, enabled }) {
	return {
		v: 1,
		id: String(id ?? ''),
		name: String(name ?? ''),
		enabled: Boolean(enabled),
		triggers: [
			{
				type: 'event',
				event_type: 'zigbee.command',
				match: { 'payload.cmd': 'toggle' },
			},
		],
		conditions: [],
		actions: [],
		mode: 'single',
	}
}

function ensureArray(v) {
	return Array.isArray(v) ? v : []
}

const UID_RE = /^0x[0-9a-fA-F]{16}$/
function normalizeUid(v) {
	return String(v ?? '').trim()
}

function isValidUid(v) {
	const s = normalizeUid(v)
	return UID_RE.test(s)
}

function normalizeEndpoint(v) {
	const n = Number(v)
	if (!Number.isFinite(n)) return NaN
	return Math.trunc(n)
}

function isValidEndpoint(v) {
	const n = normalizeEndpoint(v)
	return n >= 1 && n <= 240
}

function getDeviceOptions(devices) {
	const list = []
	for (const d of Array.isArray(devices) ? devices : []) {
		const uid = String(d?.device_uid ?? '')
		if (!uid) continue
		const name = String(d?.name ?? '')
		list.push({ uid, name, label: name ? `${name} (${uid})` : uid })
	}
	return list
}

function getEndpointsForUid(endpointsByUid, uid) {
	const u = String(uid ?? '')
	const eps = u ? endpointsByUid?.[u] : null
	return Array.isArray(eps) ? eps : []
}

function getEndpointOptions(endpointsByUid, uid) {
	const map = new Map()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		const n = Number(ep?.endpoint ?? 0)
		if (!n || map.has(n)) continue
		map.set(n, String(ep?.kind ?? '').trim())
	}
	const uniq = Array.from(map.keys()).sort((a, b) => a - b)
	const list = uniq.length ? uniq : [1]
	return list.map((ep) => ({ endpoint: ep, kind: map.get(ep) }))
}

function getEmitsByPrefix(endpointsByUid, uid, prefix) {
	const set = new Set()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		for (const s of Array.isArray(ep?.emits) ? ep.emits : []) {
			const v = String(s ?? '')
			if (!v) continue
			if (prefix) {
				if (v.startsWith(prefix)) set.add(v.slice(prefix.length))
			} else {
				set.add(v)
			}
		}
	}
	return Array.from(set).sort()
}

function getReports(endpointsByUid, uid) {
	const set = new Set()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		for (const s of Array.isArray(ep?.reports) ? ep.reports : []) {
			const v = String(s ?? '')
			if (v) set.add(v)
		}
	}
	return Array.from(set).sort()
}

function getAcceptsByPrefix(endpointsByUid, uid, endpoint, prefixes) {
	const set = new Set()
	for (const ep of getEndpointsForUid(endpointsByUid, uid)) {
		if (Number(ep?.endpoint ?? 0) !== Number(endpoint ?? 0)) continue
		for (const s of Array.isArray(ep?.accepts) ? ep.accepts : []) {
			const v = String(s ?? '')
			if (!v) continue
			if (!prefixes || prefixes.length === 0) {
				set.add(v)
				continue
			}
			for (const p of prefixes) {
				if (v.startsWith(p)) {
					set.add(v)
					break
				}
			}
		}
	}
	return Array.from(set).sort()
}

function toNumberOrString(v) {
	const s = String(v ?? '').trim()
	if (!s) return ''
	if (/^-?\d+(\.\d+)?$/.test(s)) return Number(s)
	return s
}

function AutomationEditor({
	draft,
	setDraft,
	status,
	setStatus,
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
		setField(
			'triggers',
			triggers.map((t, i) => (i === idx ? { ...(t ?? {}), ...(patch ?? {}) } : t)),
		)
	}

	const updateCondition = (idx, patch) => {
		setField(
			'conditions',
			conditions.map((c, i) => (i === idx ? { ...(c ?? {}), ...(patch ?? {}) } : c)),
		)
	}

	const updateAction = (idx, patch) => {
		setField(
			'actions',
			actions.map((a, i) => (i === idx ? { ...(a ?? {}), ...(patch ?? {}) } : a)),
		)
	}

	const removeTrigger = (idx) => setField('triggers', triggers.filter((_, i) => i !== idx))
	const removeCondition = (idx) => setField('conditions', conditions.filter((_, i) => i !== idx))
	const removeAction = (idx) => setField('actions', actions.filter((_, i) => i !== idx))

	const addTrigger = () =>
		setField('triggers', [
			...triggers,
			{ type: 'event', event_type: 'zigbee.command', match: { 'payload.cmd': 'toggle' } },
		])

	const addCondition = () => {
		const fromTrigger = String(triggers?.[0]?.match?.device_uid ?? '')
		setField('conditions', [
			...conditions,
			{ type: 'state', op: '>', ref: { device_uid: fromTrigger, key: 'temperature_c' }, value: 10 },
		])
	}

	const addAction = () =>
		setField('actions', [
			...actions,
			{ type: 'zigbee', cmd: 'onoff.toggle', device_uid: '', endpoint: 1 },
		])

	const devicesByUid = useMemo(() => {
		const m = new Map()
		for (const d of Array.isArray(devices) ? devices : []) {
			const uid = String(d?.device_uid ?? '')
			if (uid) m.set(uid, d)
		}
		return m
	}, [devices])

	const REPORT_TO_CLUSTER_ATTR = useMemo(
		() => ({
			temperature_c: { cluster: '0x0402', attr: '0x0000' },
			humidity_pct: { cluster: '0x0405', attr: '0x0000' },
			battery_pct: { cluster: '0x0001', attr: '0x0021' },
			onoff: { cluster: '0x0006', attr: '0x0000' },
			level: { cluster: '0x0008', attr: '0x0000' },
			occupancy: { cluster: '0x0406', attr: '0x0000' },
			illuminance: { cluster: '0x0400', attr: '0x0000' },
		}),
		[],
	)

	const clusterAttrToReportKey = useCallback(
		(cluster, attr) => {
			const c = String(cluster ?? '')
			const a = String(attr ?? '')
			if (!c || !a) return ''
			for (const [k, v] of Object.entries(REPORT_TO_CLUSTER_ATTR)) {
				if (v?.cluster === c && v?.attr === a) return k
			}
			return ''
		},
		[REPORT_TO_CLUSTER_ATTR],
	)

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
						<input
							value={String(draft?.id ?? '')}
							onChange={(e) => setField('id', String(e.target.value ?? ''))}
							placeholder="auto_1"
							style={{ minWidth: 220 }}
						/>
						<label className="muted" style={{ marginLeft: 10 }}>
							name
						</label>
						<input
							value={String(draft?.name ?? '')}
							onChange={(e) => setField('name', String(e.target.value ?? ''))}
							placeholder="My automation"
							style={{ minWidth: 260 }}
						/>
						<label className="row" style={{ gap: 6, marginLeft: 10 }}>
							<input
								type="checkbox"
								checked={Boolean(draft?.enabled)}
								onChange={(e) => setField('enabled', Boolean(e.target.checked))}
							/>
							<span className="muted">enabled</span>
						</label>
					</div>
				</div>
				<div className="row">
					<button onClick={onTestActions} disabled={actions.length === 0}>
						Test actions
					</button>
					<button onClick={onSave}>Save</button>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div style={{ height: 10 }} />

			<div className="row" style={{ alignItems: 'flex-start' }}>
				<div style={{ flex: 1, minWidth: 360 }}>
					<h1>Triggers</h1>
					<div className="muted">MVP: only trigger.type=event</div>
					<div style={{ height: 8 }} />
					<div className="row">
						<button onClick={addTrigger}>Add trigger</button>
					</div>
					<div style={{ height: 8 }} />
					{triggers.length === 0 ? (
						<div className="muted">No triggers</div>
					) : (
						triggers.map((t, idx) => (
							<div key={idx} className="card" style={{ padding: 10, marginBottom: 10 }}>
								<div className="row" style={{ marginBottom: 8 }}>
									<label className="muted">device</label>
									<select
										value={String(t?.match?.device_uid ?? '')}
										onChange={(e) => {
											const uid = String(e.target.value ?? '')
											if (uid) ensureEndpoints?.(uid)
											updateTrigger(idx, { match: { ...(t?.match ?? {}), device_uid: uid } })
										}}
										style={{ minWidth: 320 }}
									>
										<option value="">(any)</option>
										{deviceOptions.map((d) => (
											<option key={d.uid} value={d.uid}>
												{d.label}
											</option>
										))}
									</select>
									<span className="muted">
										{(() => {
											const uid = String(t?.match?.device_uid ?? '')
											const d = uid ? devicesByUid.get(uid) : null
											return d?.name ? `name: ${String(d.name)}` : ''
										})()}
									</span>
								</div>

								<div className="row" style={{ justifyContent: 'space-between' }}>
									<div className="row">
										<label className="muted">event_type</label>
										<select
											value={String(t?.event_type ?? 'zigbee.command')}
											onChange={(e) => {
												const nextType = String(e.target.value ?? 'zigbee.command')
												const match = { ...(t?.match ?? {}) }

												if (nextType === 'zigbee.attr_report') {
													// Attribute reports don't have payload.cmd.
													delete match['payload.cmd']
												} else if (nextType === 'zigbee.command') {
													// Commands don't have payload.attr; keep cluster optional.
													delete match['payload.attr']
												} else if (nextType === 'device.join' || nextType === 'device.leave') {
													// Device lifecycle events: drop any payload constraints.
													for (const k of Object.keys(match)) {
														if (k.startsWith('payload.')) delete match[k]
													}
												}

												updateTrigger(idx, { event_type: nextType, match })
											}}
										>
											<option value="zigbee.command">zigbee.command</option>
											<option value="zigbee.attr_report">zigbee.attr_report</option>
											<option value="device.join">device.join</option>
											<option value="device.leave">device.leave</option>
										</select>
									</div>
									<button onClick={() => removeTrigger(idx)}>Remove</button>
								</div>

								<div style={{ height: 8 }} />
								{String(t?.event_type ?? 'zigbee.command') === 'zigbee.command' ? (
									<div className="row" style={{ marginTop: 6 }}>
										<label className="muted">cmd</label>
										<select
											value={String(t?.match?.['payload.cmd'] ?? 'toggle')}
											onChange={(e) =>
												updateTrigger(idx, { match: { ...(t?.match ?? {}), 'payload.cmd': String(e.target.value ?? 'toggle') } })
											}
										>
											{(() => {
												const uid = String(t?.match?.device_uid ?? '')
												const items = getEmitsByPrefix(endpointsByUid, uid, 'onoff.')
												const list = items.length ? items : ['toggle', 'on', 'off']
												return list.map((c) => (
													<option key={c} value={c}>
														{c}
													</option>
												))
											})()}
										</select>
										<label className="muted">endpoint</label>
										<select
											value={String(t?.match?.['payload.endpoint'] ?? '')}
											onChange={(e) => {
												const v = String(e.target.value ?? '')
												const match = { ...(t?.match ?? {}) }
												if (!v) delete match['payload.endpoint']
												else match['payload.endpoint'] = Number(v)
												updateTrigger(idx, { match })
											}}
										>
											<option value="">(any)</option>
											{(() => {
												const uid = String(t?.match?.device_uid ?? '')
												return getEndpointOptions(endpointsByUid, uid).map((ep) => (
													<option key={String(ep.endpoint)} value={String(ep.endpoint)}>
														{ep.kind ? `${ep.endpoint} ${ep.kind}` : String(ep.endpoint)}
													</option>
												))
											})()}
										</select>
									</div>
								) : null}

								{String(t?.event_type ?? '') === 'zigbee.attr_report' ? (
									<div className="row" style={{ marginTop: 6 }}>
										<label className="muted">report</label>
										<select
											value={clusterAttrToReportKey(t?.match?.['payload.cluster'], t?.match?.['payload.attr'])}
											onChange={(e) => {
												const key = String(e.target.value ?? '')
												const match = { ...(t?.match ?? {}) }
												// Attribute reports don't have payload.cmd; ensure it isn't accidentally set.
												delete match['payload.cmd']
												if (!key) {
													delete match['payload.cluster']
													delete match['payload.attr']
													updateTrigger(idx, { match })
													return
												}
												const m = REPORT_TO_CLUSTER_ATTR[key]
												updateTrigger(idx, { match: { ...match, 'payload.cluster': m.cluster, 'payload.attr': m.attr } })
											}}
											style={{ minWidth: 220 }}
										>
											<option value="">(any)</option>
											{(() => {
												const uid = String(t?.match?.device_uid ?? '')
												const items = getReports(endpointsByUid, uid)
												const list = items.length ? items : Object.keys(REPORT_TO_CLUSTER_ATTR)
												return list.map((x) => (
													<option key={x} value={x}>
														{x}
													</option>
												))
											})()}
										</select>
										<label className="muted">endpoint</label>
										<select
											value={String(t?.match?.['payload.endpoint'] ?? '')}
											onChange={(e) => {
												const v = String(e.target.value ?? '')
												const match = { ...(t?.match ?? {}) }
												if (!v) delete match['payload.endpoint']
												else match['payload.endpoint'] = Number(v)
												updateTrigger(idx, { match })
											}}
										>
											<option value="">(any)</option>
											{(() => {
												const uid = String(t?.match?.device_uid ?? '')
												return getEndpointOptions(endpointsByUid, uid).map((ep) => (
													<option key={String(ep.endpoint)} value={String(ep.endpoint)}>
														{ep.kind ? `${ep.endpoint} ${ep.kind}` : String(ep.endpoint)}
													</option>
												))
											})()}
										</select>
									</div>
								) : null}

								<div style={{ height: 8 }} />
								<details>
									<summary className="muted">Advanced match fields</summary>
									<div style={{ height: 6 }} />
									<div className="muted">match (keys like payload.cmd, payload.cluster, device_uid)</div>
									<div style={{ height: 6 }} />

									{Object.keys(t?.match ?? {}).length === 0 ? (
										<div className="muted">No match fields (matches any event of event_type)</div>
									) : null}

									{Object.entries(t?.match ?? {}).map(([k, v]) => (
										<div key={k} className="row" style={{ marginTop: 6 }}>
											<input
												value={String(k ?? '')}
												onChange={(e) => {
													const nk = String(e.target.value ?? '')
													updateTrigger(idx, {
														match: Object.fromEntries(
															Object.entries(t?.match ?? {}).map(([kk, vv]) => [kk === k ? nk : kk, vv]),
														),
													})
												}}
												style={{ minWidth: 220 }}
											/>
											<input
												value={String(v ?? '')}
												onChange={(e) => {
													const nv = String(e.target.value ?? '')
													updateTrigger(idx, { match: { ...(t?.match ?? {}), [k]: toNumberOrString(nv) } })
												}}
												style={{ minWidth: 220 }}
											/>
											<button
												onClick={() => {
													const m = { ...(t?.match ?? {}) }
													delete m[k]
													updateTrigger(idx, { match: m })
												}}
											>
												x
											</button>
										</div>
									))}

									<div style={{ height: 8 }} />
									<button
										onClick={() => {
											const m = { ...(t?.match ?? {}) }
											let i = 1
											let key = 'payload.cmd'
											while (Object.prototype.hasOwnProperty.call(m, key)) {
												key = `payload.key${i++}`
											}
											m[key] = ''
											updateTrigger(idx, { match: m })
										}}
									>
										Add match field
									</button>
								</details>
							</div>
						))
					)}
				</div>

				<div style={{ flex: 1, minWidth: 360 }}>
					<h1>Conditions</h1>
					<div className="muted">MVP: state comparisons (AND across all conditions)</div>
					<div style={{ height: 8 }} />
					<div className="row">
						<button onClick={addCondition}>Add condition</button>
					</div>
					<div style={{ height: 8 }} />

					{conditions.length === 0 ? (
						<div className="muted">No conditions (always true)</div>
					) : (
						conditions.map((c, idx) => (
							<div key={idx} className="card" style={{ padding: 10, marginBottom: 10 }}>
								<div className="row" style={{ justifyContent: 'space-between' }}>
									<div className="muted">state condition</div>
									<button onClick={() => removeCondition(idx)}>Remove</button>
								</div>
								<div style={{ height: 8 }} />
								<div className="row">
									<label className="muted">device_uid</label>
									<select
										value={String(c?.ref?.device_uid ?? '')}
										onChange={(e) => updateCondition(idx, { ref: { ...(c?.ref ?? {}), device_uid: String(e.target.value ?? '') } })}
										style={{ minWidth: 320 }}
									>
										<option value="">(select device)</option>
										{deviceOptions.map((d) => (
											<option key={d.uid} value={d.uid}>
												{d.label}
											</option>
										))}
									</select>
								</div>
								<div style={{ height: 8 }} />
								<div className="row">
									<label className="muted">key</label>
									<input
										value={String(c?.ref?.key ?? '')}
										onChange={(e) => updateCondition(idx, { ref: { ...(c?.ref ?? {}), key: String(e.target.value ?? '') } })}
										placeholder="temperature_c"
										style={{ minWidth: 220 }}
									/>
									<label className="muted">op</label>
									<select value={String(c?.op ?? '==')} onChange={(e) => updateCondition(idx, { op: String(e.target.value ?? '==') })}>
										<option value="==">==</option>
										<option value="!=">!=</option>
										<option value=">">&gt;</option>
										<option value="<">&lt;</option>
										<option value=">=">&gt;=</option>
										<option value="<=">&lt;=</option>
									</select>
									<label className="muted">value</label>
									<input
										value={String(c?.value ?? '')}
										onChange={(e) => updateCondition(idx, { value: toNumberOrString(String(e.target.value ?? '')) })}
										placeholder="10"
										style={{ minWidth: 140 }}
									/>
								</div>
							</div>
						))
					)}
				</div>

				<div style={{ flex: 1, minWidth: 360 }}>
					<h1>Actions</h1>
					<div className="muted">Executed via gw_action_exec (zigbee primitives)</div>
					<div style={{ height: 8 }} />
					<div className="row">
						<button onClick={addAction}>Add action</button>
					</div>
					<div style={{ height: 8 }} />

					{actions.length === 0 ? (
						<div className="muted">No actions</div>
					) : (
						actions.map((a, idx) => {
							const cmd = String(a?.cmd ?? 'onoff.toggle')
							const isGroup = a?.group_id != null && String(a?.group_id ?? '') !== ''
							const set = (patch) => updateAction(idx, patch)
							const setCmd = (newCmd) => {
								const base = { type: 'zigbee', cmd: newCmd }
								if (newCmd.startsWith('scene.')) return set({ ...base, group_id: '0x0003', scene_id: 1 })
								if (newCmd === 'bind' || newCmd === 'unbind')
									return set({ ...base, src_device_uid: '', src_endpoint: 1, cluster_id: '0x0006', dst_device_uid: '', dst_endpoint: 1 })
								return set({ ...base, device_uid: a?.device_uid ?? '', endpoint: a?.endpoint ?? 1 })
							}

							return (
								<div key={idx} className="card" style={{ padding: 10, marginBottom: 10 }}>
									<div className="row" style={{ justifyContent: 'space-between' }}>
										<div className="row">
											<label className="muted">cmd</label>
											<select value={cmd} onChange={(e) => setCmd(String(e.target.value ?? 'onoff.toggle'))}>
												<option value="onoff.on">onoff.on</option>
												<option value="onoff.off">onoff.off</option>
												<option value="onoff.toggle">onoff.toggle</option>
												<option value="level.move_to_level">level.move_to_level</option>
												<option value="color.move_to_color_xy">color.move_to_color_xy</option>
												<option value="color.move_to_color_temperature">color.move_to_color_temperature</option>
												<option value="scene.store">scene.store</option>
												<option value="scene.recall">scene.recall</option>
												<option value="bind">bind</option>
												<option value="unbind">unbind</option>
											</select>
										</div>
										<button onClick={() => removeAction(idx)}>Remove</button>
									</div>

									<div style={{ height: 8 }} />

									{cmd === 'bind' || cmd === 'unbind' ? (
										<>
											<div className="row">
												<label className="muted">src_uid</label>
												<input value={String(a?.src_device_uid ?? '')} onChange={(e) => set({ src_device_uid: String(e.target.value ?? '') })} style={{ minWidth: 260 }} />
												<label className="muted">src_ep</label>
												<input
													value={String(a?.src_endpoint ?? 1)}
													onChange={(e) => set({ src_endpoint: Number(e.target.value ?? 1) })}
													style={{ width: 90 }}
												/>
											</div>
											<div style={{ height: 8 }} />
											<div className="row">
												<label className="muted">cluster_id</label>
												<input value={String(a?.cluster_id ?? '0x0006')} onChange={(e) => set({ cluster_id: String(e.target.value ?? '') })} style={{ minWidth: 140 }} />
											</div>
											<div style={{ height: 8 }} />
											<div className="row">
												<label className="muted">dst_uid</label>
												<input value={String(a?.dst_device_uid ?? '')} onChange={(e) => set({ dst_device_uid: String(e.target.value ?? '') })} style={{ minWidth: 260 }} />
												<label className="muted">dst_ep</label>
												<input
													value={String(a?.dst_endpoint ?? 1)}
													onChange={(e) => set({ dst_endpoint: Number(e.target.value ?? 1) })}
													style={{ width: 90 }}
												/>
											</div>
										</>
									) : cmd.startsWith('scene.') ? (
										<>
											<div className="row">
												<label className="muted">group_id</label>
												<input value={String(a?.group_id ?? '')} onChange={(e) => set({ group_id: String(e.target.value ?? '') })} placeholder="0x0003" style={{ minWidth: 140 }} />
												<label className="muted">scene_id</label>
												<input value={String(a?.scene_id ?? 1)} onChange={(e) => set({ scene_id: Number(e.target.value ?? 1) })} style={{ width: 90 }} />
											</div>
										</>
									) : (
										<>
											<div className="row">
												<label className="muted">dst</label>
												<select
													value={isGroup ? 'group' : 'device'}
													onChange={(e) => {
														const v = String(e.target.value ?? 'device')
														if (v === 'group') set({ group_id: '0x0003', device_uid: undefined })
														else set({ device_uid: String(a?.device_uid ?? ''), endpoint: Number(a?.endpoint ?? 1), group_id: undefined })
													}}
												>
													<option value="device">device</option>
													<option value="group">group</option>
												</select>
											</div>
											<div style={{ height: 8 }} />
											{isGroup ? (
												<div className="row">
													<label className="muted">group_id</label>
													<input
														value={String(a?.group_id ?? '')}
														onChange={(e) => set({ group_id: String(e.target.value ?? '') })}
														placeholder="0x0003"
														style={{ minWidth: 140 }}
													/>
												</div>
											) : (
												<div className="row">
													<label className="muted">device_uid</label>
													<select
														value={String(a?.device_uid ?? '')}
														onChange={(e) => {
															const uid = String(e.target.value ?? '')
															if (uid) ensureEndpoints?.(uid)
															set({ device_uid: uid })
														}}
														style={{ minWidth: 320 }}
													>
														<option value="">(select device)</option>
														{deviceOptions.map((d) => (
															<option key={d.uid} value={d.uid}>
																{d.label}
															</option>
														))}
													</select>
													<label className="muted">endpoint</label>
													<select value={String(a?.endpoint ?? 1)} onChange={(e) => set({ endpoint: Number(e.target.value ?? 1) })}>
														{(() => {
															const uid = String(a?.device_uid ?? '')
															return getEndpointOptions(endpointsByUid, uid).map((ep) => (
																<option key={String(ep.endpoint)} value={String(ep.endpoint)}>
																	{ep.kind ? `${ep.endpoint} ${ep.kind}` : String(ep.endpoint)}
																</option>
															))
														})()}
													</select>
												</div>
											)}

											<div className="row" style={{ marginTop: 8 }}>
												<label className="muted">capability</label>
												<select
													value={cmd}
													onChange={(e) => setCmd(String(e.target.value ?? 'onoff.toggle'))}
													style={{ minWidth: 320 }}
												>
													{(() => {
														if (isGroup) {
															return (
																<>
																	<option value="onoff.on">onoff.on</option>
																	<option value="onoff.off">onoff.off</option>
																	<option value="onoff.toggle">onoff.toggle</option>
																	<option value="level.move_to_level">level.move_to_level</option>
																	<option value="color.move_to_color_xy">color.move_to_color_xy</option>
																	<option value="color.move_to_color_temperature">color.move_to_color_temperature</option>
																</>
															)
														}
														const uid = String(a?.device_uid ?? '')
														const items = getAcceptsByPrefix(endpointsByUid, uid, a?.endpoint ?? 1, ['onoff.', 'level.', 'color.'])
														const list = items.length
															? items
															: [
																'onoff.on',
																'onoff.off',
																'onoff.toggle',
																'level.move_to_level',
																'color.move_to_color_xy',
																'color.move_to_color_temperature',
															]
														return list.map((x) => (
															<option key={x} value={x}>
																{x}
															</option>
														))
													})()}
												</select>
												<span className="muted">
													{(() => {
														const uid = String(a?.device_uid ?? '')
														const d = uid ? devicesByUid.get(uid) : null
														return d?.name ? `device: ${String(d.name)}` : ''
													})()}
												</span>
											</div>

											{cmd === 'level.move_to_level' ? (
												<div className="row" style={{ marginTop: 8 }}>
													<label className="muted">level</label>
													<input value={String(a?.level ?? 254)} onChange={(e) => set({ level: Number(e.target.value ?? 254) })} style={{ width: 110 }} />
													<label className="muted">transition_ms</label>
													<input
														value={String(a?.transition_ms ?? 0)}
														onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })}
														style={{ width: 110 }}
													/>
												</div>
											) : null}

											{cmd === 'color.move_to_color_xy' ? (
												<div className="row" style={{ marginTop: 8 }}>
													<label className="muted">x</label>
													<input value={String(a?.x ?? 30000)} onChange={(e) => set({ x: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
													<label className="muted">y</label>
													<input value={String(a?.y ?? 30000)} onChange={(e) => set({ y: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
													<label className="muted">transition_ms</label>
													<input
														value={String(a?.transition_ms ?? 0)}
														onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })}
														style={{ width: 110 }}
													/>
												</div>
											) : null}

											{cmd === 'color.move_to_color_temperature' ? (
												<div className="row" style={{ marginTop: 8 }}>
													<label className="muted">mireds</label>
													<input value={String(a?.mireds ?? 250)} onChange={(e) => set({ mireds: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
													<label className="muted">transition_ms</label>
													<input
														value={String(a?.transition_ms ?? 0)}
														onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })}
														style={{ width: 110 }}
													/>
												</div>
											) : null}
										</>
									)}
								</div>
							)
						})
					)}
				</div>
			</div>

			<div style={{ height: 10 }} />

			<details>
				<summary className="muted">Raw JSON</summary>
				<textarea
					value={jsonPreview}
					readOnly
					className="mono"
					style={{ width: '100%', minHeight: 220, marginTop: 8, padding: 10, borderRadius: 10 }}
				/>
			</details>
		</div>
	)
}

export default function Automations() {
	const { automations, devices, reloadAutomations } = useGateway()
	const [endpointsByUid, setEndpointsByUid] = useState({})
	const [status, setStatus] = useState('')
	const [draft, setDraft] = useState(null)
	const [draftStatus, setDraftStatus] = useState('')

	const validateDraft = useCallback((nextDraft) => {
		if (!nextDraft) return 'Missing automation'
		for (const [ti, t] of ensureArray(nextDraft?.triggers).entries()) {
			const uid = normalizeUid(t?.match?.device_uid ?? '')
			if (uid && !isValidUid(uid)) {
				return `Trigger ${ti + 1}: invalid device_uid`
			}
			const ep = t?.match?.['payload.endpoint']
			if (ep !== undefined && ep !== null && ep !== '') {
				if (!isValidEndpoint(ep)) return `Trigger ${ti + 1}: invalid endpoint`
			}
		}
		for (const [ci, c] of ensureArray(nextDraft?.conditions).entries()) {
			const uid = normalizeUid(c?.ref?.device_uid ?? '')
			if (!uid) return `Condition ${ci + 1}: missing device_uid`
			if (!isValidUid(uid)) return `Condition ${ci + 1}: invalid device_uid`
		}
		for (const [ai, a] of ensureArray(nextDraft?.actions).entries()) {
			const cmd = String(a?.cmd ?? '')
			if (cmd === 'bind' || cmd === 'unbind') {
				const src = normalizeUid(a?.src_device_uid ?? '')
				const dst = normalizeUid(a?.dst_device_uid ?? '')
				if (!src || !isValidUid(src)) return `Action ${ai + 1}: invalid src_device_uid`
				if (!dst || !isValidUid(dst)) return `Action ${ai + 1}: invalid dst_device_uid`
				if (!isValidEndpoint(a?.src_endpoint ?? 0)) return `Action ${ai + 1}: invalid src_endpoint`
				if (!isValidEndpoint(a?.dst_endpoint ?? 0)) return `Action ${ai + 1}: invalid dst_endpoint`
				continue
			}
			if (cmd.startsWith('scene.')) {
				const gid = String(a?.group_id ?? '').trim()
				if (!gid) return `Action ${ai + 1}: missing group_id`
				continue
			}
			const isGroup = a?.group_id != null && String(a?.group_id ?? '').trim() !== ''
			if (isGroup) {
				continue
			}
			const uid = normalizeUid(a?.device_uid ?? '')
			if (!uid) return `Action ${ai + 1}: missing device_uid`
			if (!isValidUid(uid)) return `Action ${ai + 1}: invalid device_uid`
			if (!isValidEndpoint(a?.endpoint ?? 0)) return `Action ${ai + 1}: invalid endpoint`
		}
		return ''
	}, [])

	const load = useCallback(async () => {
		setStatus('')
		try {
			await reloadAutomations()
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [reloadAutomations])

	const ensureEndpoints = useCallback(async (uid) => {
		const u = String(uid ?? '')
		if (!u) return

		setEndpointsByUid((cur) => {
			if (cur && Object.prototype.hasOwnProperty.call(cur, u)) return cur
			return { ...(cur ?? {}), [u]: null }
		})

		const dev = (Array.isArray(devices) ? devices : []).find((d) => String(d?.device_uid ?? '') === u)
		const eps = Array.isArray(dev?.endpoints) ? dev.endpoints : []
		setEndpointsByUid((cur) => ({ ...(cur ?? {}), [u]: eps }))
	}, [devices])

	const sorted = useMemo(() => {
		const items = Array.isArray(automations) ? [...automations] : []
		items.sort((a, b) => String(a?.id ?? '').localeCompare(String(b?.id ?? '')))
		return items
	}, [automations])

	const startNew = () => {
		const id = `auto_${Date.now()}`
		const name = 'New automation'
		setDraft(defaultAutomationDef({ id, name, enabled: true }))
		setDraftStatus('')
	}

	const editAutomation = async (a) => {
		try {
			const id = String(a?.id ?? '')
			if (!id) {
				setDraftStatus('Invalid automation id')
				return
			}

			setDraftStatus('Loading...')
			const base = defaultAutomationDef({ id: a?.id ?? '', name: a?.name ?? '', enabled: Boolean(a?.enabled) })
			const payload = (a?.automation && typeof a.automation === 'object') ? a.automation : null
			const merged = { ...base }

			if (payload && typeof payload === 'object') {
				if (Array.isArray(payload.triggers)) {
					merged.triggers = [...payload.triggers]
				}
				if (Array.isArray(payload.conditions)) {
					merged.conditions = [...payload.conditions]
				}
				if (Array.isArray(payload.actions)) {
					merged.actions = [...payload.actions]
				}
				merged.mode = payload.mode ?? base.mode
				merged.v = payload.v ?? base.v
			}

			merged.id = String(a?.id ?? '')
			merged.name = String(a?.name ?? '')
			merged.enabled = Boolean(a?.enabled)

			setDraft(merged)
			setDraftStatus('')

			for (const t of ensureArray(merged.triggers)) {
				const uid = String(t?.match?.device_uid ?? '')
				if (uid) ensureEndpoints(uid)
			}
			for (const act of ensureArray(merged.actions)) {
				const uid = String(act?.device_uid ?? '')
				if (uid) ensureEndpoints(uid)
			}
		} catch (e) {
			setDraftStatus(String(e?.message ?? e))
		}
	}

	const saveDraft = useCallback(async () => {
		if (!draft) return
		setDraftStatus('')
		try {
			const id = String(draft?.id ?? '').trim()
			const name = String(draft?.name ?? '').trim()
			if (!id) throw new Error('Missing id')
			if (!name) throw new Error('Missing name')
			const vErr = validateDraft(draft)
			if (vErr) throw new Error(vErr)
			const enabled = Boolean(draft?.enabled)
			console.log('Saving automation', { id, name, enabled, draft })
			await postCbor('/api/automations', { id, name, enabled, automation: draft })
			await load()
			setDraftStatus('Saved')
		} catch (e) {
			setDraftStatus(String(e?.message ?? e))
		}
	}, [draft, load])

	const testActions = useCallback(async () => {
		if (!draft) return
		setDraftStatus('')
		try {
			const actions = ensureArray(draft?.actions)
			if (actions.length === 0) throw new Error('No actions to test')
			const vErr = validateDraft({ ...draft, triggers: [], conditions: [] })
			if (vErr) throw new Error(vErr)
			await execAction(actions)
			setDraftStatus('Actions executed (queued)')
		} catch (e) {
			setDraftStatus(String(e?.message ?? e))
		}
	}, [draft])

	const toggleEnabled = async (a) => {
		setStatus('')
		try {
			const id = String(a?.id ?? '')
			if (!id) return
			await patchCbor(`/api/automations/${encodeURIComponent(id)}`, { enabled: !Boolean(a?.enabled) })
			await load()
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}

	const removeAutomation = async (a) => {
		const id = String(a?.id ?? '')
		if (!id) return
		if (!window.confirm(`Remove automation "${id}"?`)) return
		setStatus('')
		try {
			await deleteCbor(`/api/automations/${encodeURIComponent(id)}`)
			await load()
			if (String(draft?.id ?? '') === id) setDraft(null)
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}

	return (
		<div className="page">
			<div className="header">
				<div>
					<h1>Automations</h1>
					<div className="muted">MVP builder: triggers → conditions → actions.</div>
				</div>
				<div className="row">
					<button onClick={load}>Refresh</button>
					<button onClick={startNew}>New</button>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div className="split">
				<div style={{ flex: 1 }}>
					<div className="card">
						<div className="table-wrap">
							<table>
								<thead>
									<tr>
										<th>id</th>
										<th>name</th>
										<th>enabled</th>
										<th />
									</tr>
								</thead>
								<tbody>
									{sorted.length === 0 ? (
										<tr>
											<td colSpan={4} className="muted">
												No automations yet.
											</td>
										</tr>
									) : (
										sorted.map((a) => (
											<tr key={String(a?.id ?? Math.random())}>
												<td className="mono">{String(a?.id ?? '')}</td>
												<td>{String(a?.name ?? '')}</td>
												<td className="muted">{Boolean(a?.enabled) ? 'true' : 'false'}</td>
												<td className="row">
													<button onClick={() => editAutomation(a)}>Edit</button>
													<button onClick={() => toggleEnabled(a)}>{Boolean(a?.enabled) ? 'Disable' : 'Enable'}</button>
													<button onClick={() => removeAutomation(a)}>Remove</button>
												</td>
											</tr>
										))
									)}
								</tbody>
							</table>
						</div>
					</div>
				</div>

				<div style={{ flex: 2 }}>
					{draft ? (
						<AutomationEditor
							draft={draft}
							setDraft={setDraft}
							status={draftStatus}
							setStatus={setDraftStatus}
							onSave={saveDraft}
							onTestActions={testActions}
							devices={devices}
							endpointsByUid={endpointsByUid}
							ensureEndpoints={ensureEndpoints}
						/>
					) : (
						<div className="status">Select an automation or click &quot;New&quot;.</div>
					)}
				</div>
			</div>
		</div>
	)
}
