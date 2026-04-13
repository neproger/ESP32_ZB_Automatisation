//UTF-8
//ActionsSection.jsx
import { getEndpointClusterOptions, getEndpointOptions } from '../utils.js'
import { CLUSTER_NAMES, CLUSTER_COMMANDS } from '../../../proto.js'

function parseHexOrDec(s) {
  if (!s) return 0
  const str = String(s).trim()
  if (str.startsWith('0x') || str.startsWith('0X')) {
    return parseInt(str, 16)
  }
  return parseInt(str, 10) || 0
}

export default function ActionsSection({
	actions,
	deviceOptions,
	devicesByUid,
	endpointsByUid,
	ensureEndpoints,
	updateAction,
	removeAction,
	addAction,
}) {
	const getActionClusterOptions = (uid) => {
		if (!uid) return []
		return getEndpointClusterOptions(endpointsByUid, uid, 'in')
	}

	const getCommandOptions = (clusterId) => {
		if (!clusterId) return []
		return Object.entries(CLUSTER_COMMANDS[clusterId] || {}).map(([id, label]) => ({
			id: id,
			label: `${label} (0x${Number(id).toString(16).toUpperCase().padStart(2, '0')})`
		}))
	}

	// Get cmd as number from string
	const getCmdNum = (cmdStr) => {
		if (!cmdStr) return 0
		const s = String(cmdStr).trim()
		if (s.startsWith('0x') || s.startsWith('0X')) {
			return parseInt(s, 16)
		}
		return parseInt(s, 10) || 0
	}

	// Check if cmd needs parameters based on cluster + cmd
	const needsParams = (clusterId, cmdNum) => {
		if (clusterId === 0x0008) return 'level'       // Level Control
		if (clusterId === 0x0300 && cmdNum === 0x00) return 'color_xy'  // Move to color XY
		if (clusterId === 0x0300 && cmdNum === 0x01) return 'color_temp' // Move to color temp
		return null
	}

	return (
		<div style={{ flex: 1, minWidth: 360 }}>
			<div className="row" style={{ justifyContent: 'space-between', alignItems: 'baseline' }}>
				<h1>Actions</h1>
				<button onClick={addAction}>Add action</button>
			</div>
			<div className="muted">What automation executes when triggers fire and all conditions pass.</div>
			<div style={{ height: 10 }} />

			{actions.length === 0 ? (
				<div className="muted">No actions</div>
			) : (
				actions.map((a, idx) => {
					const isGroup = a?.group_id != null && String(a?.group_id ?? '') !== ''
					const set = (patch) => updateAction(idx, patch)
					const clusterId = parseHexOrDec(a?.cluster_id)
					const cmdNum = getCmdNum(a?.cmd)
					const paramType = needsParams(clusterId, cmdNum)

					return (
						<div key={idx} className="card" style={{ padding: 10, marginBottom: 10 }}>
							<div className="row" style={{ justifyContent: 'space-between' }}>
								<div className="row">
									<label className="muted">type</label>
									<select value={isGroup ? 'group' : 'device'} onChange={(e) => {
										const v = String(e.target.value ?? 'device')
										if (v === 'group') set({ group_id: '0x0003', device_uid: undefined })
										else set({ device_uid: String(a?.device_uid ?? ''), endpoint: 1, group_id: undefined, cluster_id: '0x0006', cmd: '2' })
									}}>
										<option value="device">device</option>
										<option value="group">group</option>
									</select>
								</div>
								<button onClick={() => removeAction(idx)}>Remove</button>
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
								<>
									<div className="row">
										<label className="muted">device</label>
										<select
											value={String(a?.device_uid ?? '')}
											onChange={(e) => {
												const uid = String(e.target.value ?? '')
												if (uid) ensureEndpoints?.(uid)
												set({ device_uid: uid, cluster_id: '0x0006', cmd: '2' })
											}}
											style={{ minWidth: 280 }}
										>
											<option value="">(select device)</option>
											{deviceOptions.map((d) => (
												<option key={d.uid} value={d.uid}>{d.label}</option>
											))}
										</select>
										<label className="muted">ep</label>
										<select 
											value={String(a?.endpoint ?? '')} 
											onChange={(e) => set({ endpoint: Number(e.target.value ?? 1) })}
										>
											<option value="">EP</option>
											{(() => {
												const uid = String(a?.device_uid ?? '')
												return getEndpointOptions(endpointsByUid, uid).map((ep) => (
													<option key={String(ep.endpoint)} value={String(ep.endpoint)}>
														EP{ep.endpoint}
													</option>
												))
											})()}
										</select>
									</div>
								</>
							)}

							<div style={{ height: 8 }} />

							{/* Cluster and Command selection */}
							<div className="row" style={{ flexWrap: 'wrap', gap: 8 }}>
								<div className="row">
									<label className="muted">cluster</label>
									<select
										value={String(a?.cluster_id ?? '')}
										onChange={(e) => set({ cluster_id: e.target.value, cmd: '2' })}
									>
										<option value="">cluster</option>
										{(() => {
											const uid = String(a?.device_uid ?? '')
											const epData = getActionClusterOptions(uid)
											const epNum = Number(a?.endpoint ?? 0)
											const filtered = epData.filter(ep => epNum ? ep.endpoint === epNum : true)
											const allClusters = filtered.flatMap(ep => ep.clusters || [])
											const uniq = [...new Map(allClusters.map(c => [c.id, c])).values()]
											return uniq.map(c => (
												<option key={String(c.id)} value={`0x${Number(c.id).toString(16).toUpperCase()}`}>
													{c.name}
												</option>
											))
										})()}
									</select>
								</div>

								{clusterId ? (
									<div className="row">
										<label className="muted">cmd</label>
										<select
											value={String(a?.cmd ?? '2')}
											onChange={(e) => set({ cmd: e.target.value })}
										>
											<option value="">cmd</option>
											{getCommandOptions(clusterId).map(c => (
												<option key={c.id} value={c.id}>
													{c.label}
												</option>
											))}
										</select>
									</div>
								) : null}
							</div>

							{/* Command parameters */}
							{paramType === 'level' ? (
								<div className="row" style={{ marginTop: 8 }}>
									<label className="muted">level</label>
									<input 
										value={String(a?.level ?? 254)} 
										onChange={(e) => set({ level: Number(e.target.value ?? 254) })} 
										style={{ width: 80 }} 
									/>
									<label className="muted">ms</label>
									<input 
										value={String(a?.transition_ms ?? 0)} 
										onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })} 
										style={{ width: 80 }} 
									/>
								</div>
							) : paramType === 'color_xy' ? (
								<div className="row" style={{ marginTop: 8 }}>
									<label className="muted">x</label>
									<input 
										value={String(a?.x ?? 30000)} 
										onChange={(e) => set({ x: Number(e.target.value ?? 0) })} 
										style={{ width: 80 }} 
									/>
									<label className="muted">y</label>
									<input 
										value={String(a?.y ?? 30000)} 
										onChange={(e) => set({ y: Number(e.target.value ?? 0) })} 
										style={{ width: 80 }} 
									/>
									<label className="muted">ms</label>
									<input 
										value={String(a?.transition_ms ?? 0)} 
										onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })} 
										style={{ width: 80 }} 
									/>
								</div>
							) : paramType === 'color_temp' ? (
								<div className="row" style={{ marginTop: 8 }}>
									<label className="muted">mireds</label>
									<input 
										value={String(a?.mireds ?? 250)} 
										onChange={(e) => set({ mireds: Number(e.target.value ?? 0) })} 
										style={{ width: 80 }} 
									/>
									<label className="muted">ms</label>
									<input 
										value={String(a?.transition_ms ?? 0)} 
										onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })} 
										style={{ width: 80 }} 
									/>
								</div>
							) : null}
						</div>
					)
				})
			)}
		</div>
	)
}
