//UTF-8
//ConditionsSection.jsx
import { conditionKeyType, getEndpointOptions, getReportOptions, toNumberOrString } from '../utils.js'

export default function ConditionsSection({
	conditions,
	deviceOptions,
	endpointsByUid,
	ensureEndpoints,
	updateCondition,
	removeCondition,
	addCondition,
}) {
	return (
		<div style={{ flex: 1, minWidth: 360 }}>
			<div className="row" style={{ justifyContent: 'space-between', alignItems: 'baseline' }}>
				<h1>Conditions</h1>
				<button onClick={addCondition}>Add condition</button>
			</div>
			<div className="muted">All conditions are AND. For range use two rows: {`>=`} min and {`<=`} max.</div>
			<div style={{ height: 10 }} />

			{conditions.length === 0 ? (
				<div className="muted">No conditions (always true)</div>
			) : (
				conditions.map((c, idx) => {
					const uid = String(c?.ref?.device_uid ?? '')
					const endpointValue = c?.ref?.endpoint ?? ''
					const endpointOptions = getEndpointOptions(endpointsByUid, uid)
					const reportOptions = getReportOptions(endpointsByUid, uid, endpointValue)
					const key = String(c?.ref?.key ?? '')
					const keyType = conditionKeyType(key)
					const valueOp = String(c?.op ?? '==')
					const opList = keyType === 'bool' ? ['==', '!='] : ['==', '!=', '>', '<', '>=', '<=']

					return (
						<div key={idx} className="card" style={{ padding: 10, marginBottom: 10 }}>
							<div className="row" style={{ justifyContent: 'space-between' }}>
								<div className="muted">state condition</div>
								<button onClick={() => removeCondition(idx)}>Remove</button>
							</div>
							<div style={{ height: 8 }} />
							<div className="row">
								<label className="muted">device</label>
								<select
									value={String(c?.ref?.device_uid ?? '')}
									onChange={(e) => {
										const nextUid = String(e.target.value ?? '')
										if (nextUid) ensureEndpoints?.(nextUid)
										const nextRef = { ...(c?.ref ?? {}), device_uid: nextUid }
										const eps = getEndpointOptions(endpointsByUid, nextUid)
										if (eps.length > 0) nextRef.endpoint = Number(nextRef.endpoint ?? eps[0].endpoint)
										const keys = getReportOptions(endpointsByUid, nextUid, nextRef.endpoint)
										if (!String(nextRef.key ?? '') && keys[0]?.key) nextRef.key = keys[0].key
										updateCondition(idx, { ref: nextRef })
									}}
									style={{ minWidth: 320 }}
								>
									<option value="">(select device)</option>
									{deviceOptions.map((d) => (
										<option key={d.uid} value={d.uid}>{d.label}</option>
									))}
								</select>
							</div>
							<div style={{ height: 8 }} />
							<div className="row">
								<label className="muted">endpoint</label>
								<select
									value={String(endpointValue)}
									onChange={(e) => {
										const raw = String(e.target.value ?? '')
										const nextEndpoint = raw === '' ? '' : Number(raw)
										const nextRef = { ...(c?.ref ?? {}) }
										if (nextEndpoint === '') delete nextRef.endpoint
										else nextRef.endpoint = nextEndpoint
										const keys = getReportOptions(endpointsByUid, uid, nextEndpoint)
										const currentKey = String(nextRef.key ?? '')
										if (!currentKey || !keys.some((x) => x.key === currentKey)) nextRef.key = keys[0]?.key ?? ''
										updateCondition(idx, { ref: nextRef })
									}}
								>
									<option value="">(any endpoint)</option>
									{endpointOptions.map((ep) => (
										<option key={String(ep.endpoint)} value={String(ep.endpoint)}>
											{ep.kind ? `${ep.endpoint} ${ep.kind}` : String(ep.endpoint)}
										</option>
									))}
								</select>
								<label className="muted">key</label>
								<select
									value={key}
									onChange={(e) => {
										const nextKey = String(e.target.value ?? '')
										const nextType = conditionKeyType(nextKey)
										const patch = { ref: { ...(c?.ref ?? {}), key: nextKey } }
										if (nextType === 'bool') {
											const boolValue = Boolean(c?.value)
											patch.op = (valueOp === '!=' ? '!=' : '==')
											patch.value = boolValue
										}
										updateCondition(idx, patch)
									}}
									style={{ minWidth: 260 }}
								>
									<option value="">(select key)</option>
									{reportOptions.map((item) => (
										<option key={item.key} value={item.key}>{item.label}</option>
									))}
								</select>
								<label className="muted">op</label>
								<select value={String(c?.op ?? '==')} onChange={(e) => updateCondition(idx, { op: String(e.target.value ?? '==') })}>
									{opList.map((op) => (
										<option key={op} value={op}>{op}</option>
									))}
								</select>
								<label className="muted">value</label>
								{keyType === 'bool' ? (
									<select value={String(Boolean(c?.value))} onChange={(e) => updateCondition(idx, { value: String(e.target.value ?? 'false') === 'true' })} style={{ minWidth: 140 }}>
										<option value="true">true</option>
										<option value="false">false</option>
									</select>
								) : (
									<input
										type="number"
										step="any"
										value={String(c?.value ?? '')}
										onChange={(e) => updateCondition(idx, { value: toNumberOrString(String(e.target.value ?? '')) })}
										placeholder="10"
										style={{ minWidth: 140 }}
									/>
								)}
							</div>
						</div>
					)
				})
			)}
		</div>
	)
}
