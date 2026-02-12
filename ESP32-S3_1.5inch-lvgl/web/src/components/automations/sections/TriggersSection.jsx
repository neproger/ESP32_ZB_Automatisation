//UTF-8
//TriggersSection.jsx
import { getEmitsByPrefix, getEndpointOptions, getReports, toNumberOrString } from '../utils.js'

export default function TriggersSection({
	triggers,
	deviceOptions,
	devicesByUid,
	endpointsByUid,
	ensureEndpoints,
	updateTrigger,
	removeTrigger,
	addTrigger,
	reportToClusterAttr,
	clusterAttrToReportKey,
}) {
	return (
		<div style={{ flex: 1, minWidth: 360 }}>
			<div className="row" style={{ justifyContent: 'space-between', alignItems: 'baseline' }}>
				<h1>Triggers</h1>
				<button onClick={addTrigger}>Add trigger</button>
			</div>
			<div className="muted">When automation starts: event source, endpoint, command/report filter.</div>
			<div style={{ height: 10 }} />

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
										if (nextType === 'zigbee.attr_report') delete match['payload.cmd']
										else if (nextType === 'zigbee.command') delete match['payload.attr']
										else if (nextType === 'device.join' || nextType === 'device.leave') {
											for (const k of Object.keys(match)) if (k.startsWith('payload.')) delete match[k]
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
									onChange={(e) => updateTrigger(idx, { match: { ...(t?.match ?? {}), 'payload.cmd': String(e.target.value ?? 'toggle') } })}
								>
									{(() => {
										const uid = String(t?.match?.device_uid ?? '')
										const items = getEmitsByPrefix(endpointsByUid, uid, 'onoff.')
										const list = items.length ? items : ['toggle', 'on', 'off']
										return list.map((c) => (
											<option key={c} value={c}>{c}</option>
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
										delete match['payload.cmd']
										if (!key) {
											delete match['payload.cluster']
											delete match['payload.attr']
											updateTrigger(idx, { match })
											return
										}
										const m = reportToClusterAttr[key]
										updateTrigger(idx, { match: { ...match, 'payload.cluster': m.cluster, 'payload.attr': m.attr } })
									}}
									style={{ minWidth: 220 }}
								>
									<option value="">(any)</option>
									{(() => {
										const uid = String(t?.match?.device_uid ?? '')
										const items = getReports(endpointsByUid, uid)
										const list = items.length ? items : Object.keys(reportToClusterAttr)
										return list.map((x) => (
											<option key={x} value={x}>{x}</option>
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
							<div className="muted">Keys: payload.cmd, payload.cluster, payload.attr, payload.endpoint, device_uid</div>
							<div style={{ height: 6 }} />

							{Object.keys(t?.match ?? {}).length === 0 ? <div className="muted">No extra filters</div> : null}

							{Object.entries(t?.match ?? {}).map(([k, v]) => (
								<div key={k} className="row" style={{ marginTop: 6 }}>
									<input
										value={String(k ?? '')}
										onChange={(e) => {
											const nk = String(e.target.value ?? '')
											updateTrigger(idx, { match: Object.fromEntries(Object.entries(t?.match ?? {}).map(([kk, vv]) => [kk === k ? nk : kk, vv])) })
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
									<button onClick={() => {
										const m = { ...(t?.match ?? {}) }
										delete m[k]
										updateTrigger(idx, { match: m })
									}}>x</button>
								</div>
							))}

							<div style={{ height: 8 }} />
							<button onClick={() => {
								const m = { ...(t?.match ?? {}) }
								let i = 1
								let key = 'payload.cmd'
								while (Object.prototype.hasOwnProperty.call(m, key)) key = `payload.key${i++}`
								m[key] = ''
								updateTrigger(idx, { match: m })
							}}>Add match field</button>
						</details>
					</div>
				))
			)}
		</div>
	)
}
