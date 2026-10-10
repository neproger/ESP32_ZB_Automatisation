import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { semanticAutomationPut } from '../proto.js'
import { uidHex } from '../zcl.js'
import {
  TRIGGER_KINDS, TRIGGER_OPS, TRIGGER_EDGES, CONDITION_OPS, WEEKDAY_LABELS,
  SYSTEM_EVENT_OPTIONS, ACTION_PROPERTIES, minutesToHHMM, hhmmToMinutes,
} from '../automation.js'
import { PROPERTY, ACTION, COMMAND_VALUE, propertyName, actionName } from '../semantics.js'
import { SYSTEM_DEVICE_UID } from '../system.js'
import { rgbHexToXy } from '../color.js'

const MAX_CONDITIONS = 4

function nextId(automations) {
  let max = 0n
  for (const a of automations.values()) {
    const id = a.id ?? a.key?.id
    if (id > max) max = id
  }
  return max + 1n
}

// Уникальные (ep, property) semantic-состояния устройства.
function stateOptions(states, uid) {
  const seen = new Set()
  const out = []
  for (const st of states.values()) {
    if (st.uid !== uid) continue
    const k = `${st.ep}:${st.property}`
    if (seen.has(k)) continue
    seen.add(k)
    out.push({ ep: st.ep, property: st.property })
  }
  out.sort((a, b) => a.property - b.property || a.ep - b.ep)
  return out
}

export default function AutomationForm({ id, devices, automations, onClose }) {
  const s = useStore()
  const existing = id != null ? s.semAutomations.get(String(id))?.rule : null

  const devOptions = (anyLabel) => [
    ...(anyLabel ? [<option key="0" value="0">{anyLabel}</option>] : []),
    ...devices.map((d) => (
      <option key={d.key.uid.toString()} value={d.key.uid.toString()}>
        {d.record.name || d.record.model || uidHex(d.key.uid)}
      </option>
    )),
  ]

  const [enabled, setEnabled] = useState(existing ? !!existing.enabled : true)
  const [triggerKind, setTriggerKind] = useState(existing ? existing.trigger.kind : 1)
  const [triggerUid, setTriggerUid] = useState(existing?.trigger.deviceUid ? existing.trigger.deviceUid.toString() : '0')
  const [eventId, setEventId] = useState(existing?.trigger.eventId ?? SYSTEM_EVENT_OPTIONS[0][0])
  const [timeStr, setTimeStr] = useState(minutesToHHMM(existing?.trigger.minutesOfDay ?? 420))
  const [weekdayMask, setWeekdayMask] = useState(existing?.trigger.weekdayMask ?? 0x7f)
  const [stateEp, setStateEp] = useState(existing?.trigger.endpoint ?? 0)
  const [stateProperty, setStateProperty] = useState(existing?.trigger.property ?? 0)
  const [stateOp, setStateOp] = useState(existing?.trigger.op ?? 1)
  const [stateEdge, setStateEdge] = useState(existing?.trigger.edge ?? 1)
  const [stateValue, setStateValue] = useState(existing?.trigger.value ?? 0)

  const [conditions, setConditions] = useState(
    existing ? existing.conditions.map((c) => ({ ...c, deviceUid: c.deviceUid.toString() })) : [],
  )

  const [actionUid, setActionUid] = useState(existing?.action.deviceUid ? existing.action.deviceUid.toString() : '0')
  const [actionEp, setActionEp] = useState(existing?.action.endpoint ?? 0)
  const [actionProperty, setActionProperty] = useState(existing?.action.property ?? PROPERTY.POWER)
  const [actionAction, setActionAction] = useState(existing?.action.action ?? ACTION.ON)
  const [actionValue, setActionValue] = useState(existing?.action.value ?? 70)
  const [actionColor, setActionColor] = useState('#ffcc88')

  const isSystemTrigger = triggerUid !== '0' && BigInt(triggerUid) === SYSTEM_DEVICE_UID
  const trigStateOpts = stateOptions(s.semStates, triggerUid !== '0' ? BigInt(triggerUid) : 0n)

  const propDef = ACTION_PROPERTIES.find((p) => p.id === actionProperty) || ACTION_PROPERTIES[0]
  const pickProperty = (v) => {
    const p = ACTION_PROPERTIES.find((x) => x.id === Number(v)) || ACTION_PROPERTIES[0]
    setActionProperty(p.id)
    setActionAction(p.actions[0])
  }

  const updateCondition = (i, patch) => setConditions((cs) => cs.map((c, j) => (j === i ? { ...c, ...patch } : c)))
  const addCondition = () => {
    if (conditions.length >= MAX_CONDITIONS) return
    setConditions((cs) => [...cs, { deviceUid: '0', endpoint: 0, property: PROPERTY.POWER, op: 1, value: 1, value2: 0 }])
  }

  const submit = (e) => {
    e.preventDefault()
    const targetId = id != null ? BigInt(id) : nextId(automations)
    const trigger =
      Number(triggerKind) === 1
        ? { kind: 1, minutesOfDay: hhmmToMinutes(timeStr), weekdayMask }
        : Number(triggerKind) === 0
          ? { kind: 0, deviceUid: BigInt(triggerUid), eventId: Number(eventId) }
          : {
              kind: 2,
              deviceUid: BigInt(triggerUid),
              endpoint: Number(stateEp),
              property: Number(stateProperty),
              op: Number(stateOp),
              edge: Number(stateEdge),
              value: Number(stateValue) || 0,
            }

    let action
    if (actionAction === ACTION.SET && propDef.value === 'color') {
      const { x, y } = rgbHexToXy(actionColor)
      action = { deviceUid: BigInt(actionUid), endpoint: Number(actionEp), property: actionProperty, action: actionAction, valueKind: COMMAND_VALUE.XY, x: x / 65535, y: y / 65535 }
    } else if (actionAction === ACTION.SET) {
      action = { deviceUid: BigInt(actionUid), endpoint: Number(actionEp), property: actionProperty, action: actionAction, valueKind: COMMAND_VALUE.SCALAR, value: Number(actionValue) || 0 }
    } else {
      action = { deviceUid: BigInt(actionUid), endpoint: Number(actionEp), property: actionProperty, action: actionAction, valueKind: COMMAND_VALUE.NONE }
    }

    store.send(
      semanticAutomationPut(targetId, {
        enabled,
        trigger,
        conditions: conditions.map((c) => ({
          deviceUid: BigInt(c.deviceUid || 0),
          endpoint: Number(c.endpoint) || 0,
          property: Number(c.property) || 0,
          op: Number(c.op) || 1,
          value: Number(c.value) || 0,
          value2: Number(c.value2) || 0,
        })),
        action,
      }),
    )
    onClose()
  }

  return (
    <form className="form-grid" onSubmit={submit}>
      <label className="af-line"><input type="checkbox" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} /> Включено</label>

      <div className="af-line">Триггер:
        <select value={triggerKind} onChange={(e) => setTriggerKind(Number(e.target.value))}>
          {TRIGGER_KINDS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
        {Number(triggerKind) === 1 ? (
          <>
            <select value={Math.floor(hhmmToMinutes(timeStr) / 60)}
              onChange={(e) => setTimeStr(`${String(Number(e.target.value)).padStart(2, '0')}:${String(hhmmToMinutes(timeStr) % 60).padStart(2, '0')}`)}>
              {Array.from({ length: 24 }, (_, h) => <option key={h} value={h}>{String(h).padStart(2, '0')}</option>)}
            </select>
            <span>:</span>
            <select value={hhmmToMinutes(timeStr) % 60}
              onChange={(e) => setTimeStr(`${String(Math.floor(hhmmToMinutes(timeStr) / 60)).padStart(2, '0')}:${String(Number(e.target.value)).padStart(2, '0')}`)}>
              {Array.from({ length: 60 }, (_, m) => <option key={m} value={m}>{String(m).padStart(2, '0')}</option>)}
            </select>
            <span className="days">
              {WEEKDAY_LABELS.map((d, i) => (
                <label key={d} className="day">
                  <input type="checkbox" checked={(weekdayMask >> i) & 1}
                    onChange={(e) => setWeekdayMask((m) => (e.target.checked ? (m | (1 << i)) : (m & ~(1 << i))))} />
                  {d}
                </label>
              ))}
            </span>
          </>
        ) : Number(triggerKind) === 0 ? (
          <>
            <select value={triggerUid} onChange={(e) => setTriggerUid(e.target.value)}>
              <option value="0">выберите устройство</option>
              {devices.map((d) => <option key={d.key.uid.toString()} value={d.key.uid.toString()}>{d.record.name || d.record.model || uidHex(d.key.uid)}</option>)}
            </select>
            {isSystemTrigger ? (
              <select value={eventId} onChange={(e) => setEventId(Number(e.target.value))}>
                {SYSTEM_EVENT_OPTIONS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
            ) : (
              <span className="muted">события устройства пока не представимы</span>
            )}
          </>
        ) : (
          <>
            <select value={triggerUid} onChange={(e) => { setTriggerUid(e.target.value); setStateEp(0); setStateProperty(0) }}>
              <option value="0">выберите устройство</option>
              {devices.map((d) => <option key={d.key.uid.toString()} value={d.key.uid.toString()}>{d.record.name || d.record.model || uidHex(d.key.uid)}</option>)}
            </select>
            <select value={stateProperty ? `${stateEp}:${stateProperty}` : ''}
              onChange={(e) => { const [ep, p] = e.target.value.split(':').map(Number); setStateEp(ep); setStateProperty(p) }}>
              <option value="">(свойство)</option>
              {trigStateOpts.map((o) => <option key={`${o.ep}:${o.property}`} value={`${o.ep}:${o.property}`}>{propertyName(o.property)} (EP{o.ep})</option>)}
            </select>
            <select value={stateOp} onChange={(e) => setStateOp(Number(e.target.value))}>
              {TRIGGER_OPS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            <input className="ep" type="number" step="any" value={stateValue} onChange={(e) => setStateValue(e.target.value)} />
            <select value={stateEdge} onChange={(e) => setStateEdge(Number(e.target.value))}>
              {TRIGGER_EDGES.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </>
        )}
      </div>

      <label className="af-line">Действие:
        <select value={actionUid} onChange={(e) => setActionUid(e.target.value)}>
          {Number(triggerKind) !== 1 && <option value="0">то же, что триггер</option>}
          {devices.map((d) => <option key={d.key.uid.toString()} value={d.key.uid.toString()}>{d.record.name || d.record.model || uidHex(d.key.uid)}</option>)}
        </select>
        EP <input className="ep" type="number" min="0" max="255" value={actionEp} onChange={(e) => setActionEp(e.target.value)} />
        <select value={actionProperty} onChange={(e) => pickProperty(e.target.value)}>
          {ACTION_PROPERTIES.map((p) => <option key={p.id} value={p.id}>{propertyName(p.id)}</option>)}
        </select>
        <select value={actionAction} onChange={(e) => setActionAction(Number(e.target.value))}>
          {propDef.actions.map((a) => <option key={a} value={a}>{actionName(a)}</option>)}
        </select>
      </label>

      {actionAction === ACTION.SET && propDef.value === 'color' && (
        <label className="af-line">Цвет: <input type="color" value={actionColor} onChange={(e) => setActionColor(e.target.value)} /></label>
      )}
      {actionAction === ACTION.SET && propDef.value && propDef.value !== 'color' && (
        <label className="af-line">Значение:
          <input className="ep" type="number" step="any" value={actionValue} onChange={(e) => setActionValue(e.target.value)} />
          <span className="muted">{propDef.value === 'percent' ? '%' : propDef.value === 'kelvin' ? 'K' : '°'}</span>
        </label>
      )}

      <div className="af-section">
        <div className="af-section-head">
          <span>Условия (все должны выполняться)</span>
          <button type="button" onClick={addCondition} disabled={conditions.length >= MAX_CONDITIONS}>+ Условие</button>
        </div>
        {conditions.length === 0 && <div className="muted">нет — срабатывает всегда</div>}
        {conditions.map((c, i) => {
          const opts = stateOptions(s.semStates, c.deviceUid && c.deviceUid !== '0' ? BigInt(c.deviceUid) : (triggerUid !== '0' ? BigInt(triggerUid) : 0n))
          return (
            <div className="af-cond" key={i}>
              <select value={c.deviceUid} onChange={(e) => updateCondition(i, { deviceUid: e.target.value, endpoint: 0, property: 0 })}>
                {devOptions('то же, что триггер')}
              </select>
              <select value={c.property ? `${c.endpoint}:${c.property}` : ''}
                onChange={(e) => { const [ep, p] = e.target.value.split(':').map(Number); updateCondition(i, { endpoint: ep, property: p }) }}>
                <option value="">(свойство)</option>
                {opts.map((o) => <option key={`${o.ep}:${o.property}`} value={`${o.ep}:${o.property}`}>{propertyName(o.property)} (EP{o.ep})</option>)}
              </select>
              <select value={c.op} onChange={(e) => updateCondition(i, { op: Number(e.target.value) })}>
                {CONDITION_OPS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
              {c.op === 8 ? (
                <>
                  <input className="ep" type="number" step="any" value={c.value} onChange={(e) => updateCondition(i, { value: e.target.value })} />
                  <span className="muted">…</span>
                  <input className="ep" type="number" step="any" value={c.value2 ?? 0} onChange={(e) => updateCondition(i, { value2: e.target.value })} />
                </>
              ) : (
                <input className="ep" type="number" step="any" value={c.value} onChange={(e) => updateCondition(i, { value: e.target.value })} />
              )}
              <button type="button" className="ghost danger" onClick={() => setConditions((cs) => cs.filter((_, j) => j !== i))}>✕</button>
            </div>
          )
        })}
      </div>

      <div className="af-line">
        <button type="submit">Сохранить</button>
        <button type="button" onClick={onClose}>Отмена</button>
      </div>
    </form>
  )
}
