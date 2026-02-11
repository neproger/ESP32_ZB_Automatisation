import { useCallback, useMemo, useState } from 'react'
import { postCbor, patchCbor, deleteCbor, execAction } from '../api.js'
import { useGateway } from '../gateway.jsx'
import AutomationEditor from '../components/automations/AutomationEditor.jsx'
import AutomationList from '../components/automations/AutomationList.jsx'
import {
	defaultAutomationDef,
	ensureArray,
	normalizeUid,
	isValidUid,
	isValidEndpoint,
	conditionKeyType,
} from '../components/automations/utils.js'

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
			const endpoint = c?.ref?.endpoint
			if (endpoint !== undefined && endpoint !== null && endpoint !== '') {
				if (!isValidEndpoint(endpoint)) return `Condition ${ci + 1}: invalid endpoint`
			}
			const key = String(c?.ref?.key ?? '').trim()
			if (!key) return `Condition ${ci + 1}: missing key`
			const op = String(c?.op ?? '')
			const keyType = conditionKeyType(key)
			if (keyType === 'bool') {
				if (op !== '==' && op !== '!=') return `Condition ${ci + 1}: op must be == or != for boolean key`
			} else if (!['==', '!=', '>', '<', '>=', '<='].includes(op)) {
				return `Condition ${ci + 1}: invalid op`
			}
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
			if (isGroup) continue
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
				if (Array.isArray(payload.triggers)) merged.triggers = [...payload.triggers]
				if (Array.isArray(payload.conditions)) merged.conditions = [...payload.conditions]
				if (Array.isArray(payload.actions)) merged.actions = [...payload.actions]
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
			for (const c of ensureArray(merged.conditions)) {
				const uid = String(c?.ref?.device_uid ?? '')
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
			await postCbor('/api/automations', { id, name, enabled, automation: draft })
			setDraftStatus('Saved (waiting WS sync)')
		} catch (e) {
			setDraftStatus(String(e?.message ?? e))
		}
	}, [draft, validateDraft])

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
	}, [draft, validateDraft])

	const toggleEnabled = async (a) => {
		setStatus('')
		try {
			const id = String(a?.id ?? '')
			if (!id) return
			await patchCbor(`/api/automations/${encodeURIComponent(id)}`, { enabled: !Boolean(a?.enabled) })
			setStatus('Updated (waiting WS sync)')
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
			setStatus('Removed (waiting WS sync)')
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
					<div className="muted">MVP builder: triggers to conditions to actions.</div>
				</div>
				<div className="row">
					<button onClick={load}>Refresh</button>
					<button onClick={startNew}>New</button>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div className="split">
				<div style={{ flex: 1 }}>
					<AutomationList sorted={sorted} onEdit={editAutomation} onToggleEnabled={toggleEnabled} onRemove={removeAutomation} />
				</div>

				<div style={{ flex: 2 }}>
					{draft ? (
						<AutomationEditor
							draft={draft}
							setDraft={setDraft}
							status={draftStatus}
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
