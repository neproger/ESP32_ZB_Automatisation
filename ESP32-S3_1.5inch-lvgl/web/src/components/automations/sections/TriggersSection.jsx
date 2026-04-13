//UTF-8
//TriggersSection.jsx
import { EVT_ZIGBEE_ATTR_REPORT, EVT_ZIGBEE_COMMAND, EVT_ZIGBEE_DEVICE_JOIN, EVT_ZIGBEE_DEVICE_LEAVE } from '../../../eventNames.js'
import { getEndpointClusterOptions, getEndpointOptions, getReports, toNumberOrString } from '../utils.js'
import { CLUSTER_NAMES, CLUSTER_COMMANDS, OUT_CLUSTER_EVENTS } from '../../../proto.js'

function getClusterName(id) {
  return CLUSTER_NAMES[id] || `Cluster 0x${Number(id).toString(16).toUpperCase().padStart(4, '0')}`
}

function getCommandsForCluster(clusterId) {
  return CLUSTER_COMMANDS[clusterId] || OUT_CLUSTER_EVENTS[clusterId] || null
}

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
	// Get available clusters for trigger (out_clusters from endpoints)
	const getTriggerClusterOptions = (uid) => {
		if (!uid) return []
		return getEndpointClusterOptions(endpointsByUid, uid, 'out')
	}

	// Parse selected cluster from match
	const parseSelectedCluster = (match) => {
		const cluster = Number(match?.['payload.cluster'] || 0)
		const cmd = String(match?.['payload.cmd'] || '')
		return { cluster, cmd }
	}

	// Build command options for selected cluster
	const getCommandOptions = (clusterId, direction) => {
		if (!clusterId) return []
		const cmds = direction === 'out' 
			? (OUT_CLUSTER_EVENTS[clusterId] || CLUSTER_COMMANDS[clusterId])
			: CLUSTER_COMMANDS[clusterId]
		if (!cmds) return []
		return Object.entries(cmds).map(([id, label]) => ({
			id: Number(id),
			label: `${label} (0x${Number(id).toString(16).toUpperCase().padStart(2, '0')})`
		}))
	}

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
						</div>

						<div className="row" style={{ justifyContent: 'space-between' }}>
							<div className="row">
								<label className="muted">event_type</label>
								<select
									value={String(t?.event_type ?? EVT_ZIGBEE_COMMAND)}
									onChange={(e) => {
										const nextType = String(e.target.value ?? EVT_ZIGBEE_COMMAND)
										const match = { ...(t?.match ?? {}) }
										if (nextType === EVT_ZIGBEE_ATTR_REPORT) delete match['payload.cmd']
										else if (nextType === EVT_ZIGBEE_COMMAND) delete match['payload.attr']
										else if (nextType === EVT_ZIGBEE_DEVICE_JOIN || nextType === EVT_ZIGBEE_DEVICE_LEAVE) {
											for (const k of Object.keys(match)) if (k.startsWith('payload.')) delete match[k]
										}
										updateTrigger(idx, { event_type: nextType, match })
									}}
								>
									<option value={EVT_ZIGBEE_COMMAND}>{EVT_ZIGBEE_COMMAND}</option>
									<option value={EVT_ZIGBEE_ATTR_REPORT}>{EVT_ZIGBEE_ATTR_REPORT}</option>
									<option value={EVT_ZIGBEE_DEVICE_JOIN}>{EVT_ZIGBEE_DEVICE_JOIN}</option>
									<option value={EVT_ZIGBEE_DEVICE_LEAVE}>{EVT_ZIGBEE_DEVICE_LEAVE}</option>
								</select>
							</div>
							<button onClick={() => removeTrigger(idx)}>Remove</button>
						</div>

						<div style={{ height: 8 }} />
						{String(t?.event_type ?? EVT_ZIGBEE_COMMAND) === EVT_ZIGBEE_COMMAND ? (
							<div className="row" style={{ marginTop: 6, flexWrap: 'wrap', gap: 8 }}>
								{/* Endpoint selector */}
								<div className="row">
									<label className="muted">endpoint</label>
									<select
										value={String(t?.match?.['payload.endpoint'] ?? '')}
										onChange={(e) => {
											const v = String(e.target.value ?? '')
											const match = { ...(t?.match ?? {}) }
											if (!v) {
												delete match['payload.endpoint']
												delete match['payload.cluster']
												delete match['payload.cmd']
											} else {
												match['payload.endpoint'] = Number(v)
											}
											updateTrigger(idx, { match })
										}}
									>
										<option value="">EP (any)</option>
										{(() => {
											const uid = String(t?.match?.device_uid ?? '')
											return getEndpointOptions(endpointsByUid, uid).map((ep) => (
												<option key={String(ep.endpoint)} value={String(ep.endpoint)}>
													EP{ep.endpoint}
												</option>
											))
										})()}
									</select>
								</div>

								{/* Cluster selector */}
								<div className="row">
									<label className="muted">cluster</label>
									<select
										value={t?.match?.['payload.cluster'] ? `0x${Number(t?.match?.['payload.cluster']).toString(16).toUpperCase()}` : ''}
										onChange={(e) => {
											const v = String(e.target.value ?? '')
											const match = { ...(t?.match ?? {}) }
											if (!v) {
												delete match['payload.cluster']
												delete match['payload.cmd']
											} else {
												const num = v.startsWith('0x') ? parseInt(v, 16) : Number(v)
												match['payload.cluster'] = num
												delete match['payload.cmd']
											}
											updateTrigger(idx, { match })
										}}
									>
										<option value="">cluster</option>
										{(() => {
											const uid = String(t?.match?.device_uid ?? '')
											const epData = getTriggerClusterOptions(uid)
											const allClusters = epData.flatMap(ep => ep.clusters || [])
											const uniq = [...new Map(allClusters.map(c => [c.id, c])).values()]
											return uniq.map(c => (
												<option key={String(c.id)} value={`0x${Number(c.id).toString(16).toUpperCase()}`}>
													{c.name}
												</option>
											))
										})()}
									</select>
								</div>

								{/* Command selector */}
								{Number(t?.match?.['payload.cluster']) ? (
									<div className="row">
										<label className="muted">cmd</label>
										<select
											value={String(t?.match?.['payload.cmd'] ?? '')}
											onChange={(e) => {
												const v = String(e.target.value ?? '')
												const match = { ...(t?.match ?? {}) }
												if (!v) {
													delete match['payload.cmd']
												} else {
													match['payload.cmd'] = v
												}
												updateTrigger(idx, { match })
											}}
										>
											<option value="">cmd</option>
											{(() => {
												const clusterId = Number(t?.match?.['payload.cluster'])
												const cmdOpts = getCommandOptions(clusterId, 'out')
												return cmdOpts.map(c => (
													<option key={String(c.id)} value={String(c.id)}>
														{c.label}
													</option>
												))
											})()}
										</select>
									</div>
								) : null}
							</div>
						) : null}

						{String(t?.event_type ?? '') === EVT_ZIGBEE_ATTR_REPORT ? (
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
