//UTF-8
//ActionsSection.jsx
import { getAcceptsByPrefix, getEndpointOptions } from '../utils.js'

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
					const cmd = String(a?.cmd ?? 'onoff.toggle')
					const isGroup = a?.group_id != null && String(a?.group_id ?? '') !== ''
					const set = (patch) => updateAction(idx, patch)
					const setCmd = (newCmd) => {
						const base = { type: 'zigbee', cmd: newCmd }
						if (newCmd.startsWith('scene.')) return set({ ...base, group_id: '0x0003', scene_id: 1 })
						if (newCmd === 'bind' || newCmd === 'unbind') {
							return set({ ...base, src_device_uid: '', src_endpoint: 1, cluster_id: '0x0006', dst_device_uid: '', dst_endpoint: 1 })
						}
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
										<input value={String(a?.src_endpoint ?? 1)} onChange={(e) => set({ src_endpoint: Number(e.target.value ?? 1) })} style={{ width: 90 }} />
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
										<input value={String(a?.dst_endpoint ?? 1)} onChange={(e) => set({ dst_endpoint: Number(e.target.value ?? 1) })} style={{ width: 90 }} />
									</div>
								</>
							) : cmd.startsWith('scene.') ? (
								<div className="row">
									<label className="muted">group_id</label>
									<input value={String(a?.group_id ?? '')} onChange={(e) => set({ group_id: String(e.target.value ?? '') })} placeholder="0x0003" style={{ minWidth: 140 }} />
									<label className="muted">scene_id</label>
									<input value={String(a?.scene_id ?? 1)} onChange={(e) => set({ scene_id: Number(e.target.value ?? 1) })} style={{ width: 90 }} />
								</div>
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
											<input value={String(a?.group_id ?? '')} onChange={(e) => set({ group_id: String(e.target.value ?? '') })} placeholder="0x0003" style={{ minWidth: 140 }} />
										</div>
									) : (
										<div className="row">
											<label className="muted">device</label>
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
													<option key={d.uid} value={d.uid}>{d.label}</option>
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
										<select value={cmd} onChange={(e) => setCmd(String(e.target.value ?? 'onoff.toggle'))} style={{ minWidth: 320 }}>
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
													: ['onoff.on', 'onoff.off', 'onoff.toggle', 'level.move_to_level', 'color.move_to_color_xy', 'color.move_to_color_temperature']
												return list.map((x) => <option key={x} value={x}>{x}</option>)
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
											<input value={String(a?.transition_ms ?? 0)} onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
										</div>
									) : null}

									{cmd === 'color.move_to_color_xy' ? (
										<div className="row" style={{ marginTop: 8 }}>
											<label className="muted">x</label>
											<input value={String(a?.x ?? 30000)} onChange={(e) => set({ x: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
											<label className="muted">y</label>
											<input value={String(a?.y ?? 30000)} onChange={(e) => set({ y: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
											<label className="muted">transition_ms</label>
											<input value={String(a?.transition_ms ?? 0)} onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
										</div>
									) : null}

									{cmd === 'color.move_to_color_temperature' ? (
										<div className="row" style={{ marginTop: 8 }}>
											<label className="muted">mireds</label>
											<input value={String(a?.mireds ?? 250)} onChange={(e) => set({ mireds: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
											<label className="muted">transition_ms</label>
											<input value={String(a?.transition_ms ?? 0)} onChange={(e) => set({ transition_ms: Number(e.target.value ?? 0) })} style={{ width: 110 }} />
										</div>
									) : null}
								</>
							)}
						</div>
					)
				})
			)}
		</div>
	)
}
