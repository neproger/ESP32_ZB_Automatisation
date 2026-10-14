import { useEffect, useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { automationPut, ENTITY_SOURCE } from '../proto.js'
import {
  TRIGGER_KINDS, TRIGGER_OPS, TRIGGER_EDGES, CONDITION_OPS, WEEKDAY_LABELS,
  SYSTEM_EVENT_OPTIONS, ACTION_PROPERTIES, minutesToHHMM, hhmmToMinutes,
} from '../automation.js'
import { PROPERTY, ACTION, COMMAND_VALUE, propertyName, actionName, eventName } from '../semantics.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'
import { ENTITY_SYSTEM } from '../system.js'
import Switch from './Switch.jsx'

const MAX_TRIGGERS = 4
const MAX_ACTIONS = 4
const MAX_CONDITIONS = 4
const SRC = 'src' // UI-сентинел для HA_ENTITY_ID_SOURCE

function nextId(automations) {
  let max = 0n
  for (const a of automations.values()) {
    const id = a.id ?? a.key?.id
    if (id > max) max = id
  }
  return max + 1n
}

// Свойства Entity: capabilities + уже известные значения. Identity — entity_id.
function propsOf(entityStr) {
  if (!entityStr || entityStr === SRC) return []
  const id = BigInt(entityStr)
  const set = new Map()
  for (const cap of store.capabilities(id)) set.set(cap.property, { property: cap.property, actions: cap.actions })
  for (const st of store.entityProperties(id)) {
    if (!set.has(st.property)) set.set(st.property, { property: st.property, actions: [] })
  }
  return [...set.values()].sort((a, b) => a.property - b.property)
}

const newTrigger = () => ({ kind: 1, entity: SRC, eventId: SYSTEM_EVENT_OPTIONS[0][0], timeStr: '07:00', weekdayMask: 0x7f, property: 0, op: 1, edge: 0, value: 0 })
const newAction = () => ({ entity: SRC, property: PROPERTY.POWER, action: ACTION.ON, value: 70, color: '#ffcc88' })

export default function AutomationForm({ id, automations, onClose }) {
  const s = useStore()
  const existing = id != null ? s.semAutomations.get(String(id))?.rule : null
  const ents = [...s.semEntities.values()]

  const toEntity = (v) => (v === SRC ? ENTITY_SOURCE : BigInt(v))
  const fromEntity = (e) => (e == null || BigInt(e) === ENTITY_SOURCE ? SRC : BigInt(e).toString())
  const triggerFromWire = (t) => ({
    kind: t.kind,
    entity: fromEntity(t.entity),
    eventId: t.eventId,
    timeStr: minutesToHHMM(t.minutesOfDay ?? 420),
    weekdayMask: t.weekdayMask ?? 0x7f,
    property: t.property ?? 0,
    op: t.op ?? 1,
    edge: t.edge ?? 0,
    value: t.value ?? 0,
  })
  const actionFromWire = (w) => ({
    entity: fromEntity(w.entity),
    property: w.property,
    action: w.action,
    value: w.valueKind === 2 ? 70 : (w.value ?? 70),
    color: w.valueKind === 2 ? xyToRgbHex((w.x ?? 0) * 65535, (w.y ?? 0) * 65535) : '#ffcc88',
  })

  const [enabled, setEnabled] = useState(existing ? !!existing.enabled : true)
  const [triggers, setTriggers] = useState(() =>
    existing?.triggers?.length ? existing.triggers.map(triggerFromWire) : [newTrigger()])
  const [actions, setActions] = useState(() =>
    existing?.actions?.length ? existing.actions.map(actionFromWire) : [newAction()])
  const [conditions, setConditions] = useState(
    existing ? existing.conditions.map((c) => ({ ...c, entity: fromEntity(c.entity) })) : [],
  )

  if (ents.length === 0 && !existing) {
    return (
      <div className="form-grid">
        <p className="muted">нет сущностей — сначала добавьте устройства</p>
        <button type="button" onClick={onClose}>Закрыть</button>
      </div>
    )
  }

  const entityLabel = (e) => {
    if (e.name) return e.name
    const caps = store.capabilities(e.id)
    const hint = caps.length > 0 ? ` · ${propertyName(caps[0].property)}` : ''
    return `#${e.id.toString().slice(-4)}${hint}`
  }
  const hasEvents = (e) => store.entityEvents(e.id).length > 0
  const hasProperties = (e) => store.capabilities(e.id).length > 0 || store.entityProperties(e.id).length > 0
  const hasActions = (e) => store.capabilities(e.id).some((c) => (c.actions?.length ?? 0) > 0)
  const eventEntities = ents.filter(hasEvents)
  const stateEntities = ents.filter(hasProperties)
  const actionEntities = ents.some(hasActions) ? ents.filter(hasActions) : ents

  const entityOptions = (list, includeSource, sourceLabel) => {
    const nodes = []
    if (includeSource) nodes.push(<option key="src" value={SRC}>{sourceLabel}</option>)
    const groups = new Map()
    for (const e of list) {
      const dk = e.deviceId?.toString() ?? ''
      if (!groups.has(dk)) groups.set(dk, [])
      groups.get(dk).push(e)
    }
    for (const [dk, group] of groups) {
      const dn = s.semDevices.get(dk)?.name || (dk ? 'устройство ' + dk.slice(-6) : 'без устройства')
      nodes.push(
        <optgroup key={'g' + dk} label={dn}>
          {group.map((e) => <option key={e.id.toString()} value={e.id.toString()}>{entityLabel(e)}</option>)}
        </optgroup>,
      )
    }
    return nodes
  }

  const updateTrigger = (i, patch) => setTriggers((ts) => ts.map((t, j) => (j === i ? { ...t, ...patch } : t)))
  const updateAction = (i, patch) => setActions((as) => as.map((a, j) => (j === i ? { ...a, ...patch } : a)))

  const actionOptionsFor = (entityStr) =>
    entityStr === SRC ? ACTION_PROPERTIES.map((p) => ({ property: p.id, actions: p.actions })) : propsOf(entityStr)
  const valueShapeOf = (property) => (ACTION_PROPERTIES.find((p) => p.id === Number(property)) || {}).value
  const pickActionProperty = (i, v) => {
    const property = Number(v)
    const opt = actionOptionsFor(actions[i].entity).find((p) => p.property === property)
    updateAction(i, { property, action: opt?.actions?.[0] ?? ACTION.SET })
  }

  const updateCondition = (i, patch) => setConditions((cs) => cs.map((c, j) => (j === i ? { ...c, ...patch } : c)))
  const addCondition = () => {
    if (conditions.length >= MAX_CONDITIONS) return
    setConditions((cs) => [...cs, { entity: SRC, property: PROPERTY.POWER, op: 1, value: 1, value2: 0 }])
  }

  const toTriggerWire = (t) =>
    Number(t.kind) === 1
      ? { kind: 1, minutesOfDay: hhmmToMinutes(t.timeStr), weekdayMask: t.weekdayMask }
      : Number(t.kind) === 0
        ? { kind: 0, entity: toEntity(t.entity), eventId: Number(t.eventId) }
        : { kind: 2, entity: toEntity(t.entity), property: Number(t.property) || 0, op: Number(t.op) || 1, edge: Number(t.edge) || 0, value: Number(t.value) || 0 }

  const toActionWire = (a) => {
    const shape = valueShapeOf(a.property)
    if (a.action === ACTION.SET && shape === 'color') {
      const { x, y } = rgbHexToXy(a.color)
      return { entity: toEntity(a.entity), property: Number(a.property), action: Number(a.action), valueKind: COMMAND_VALUE.XY, x: x / 65535, y: y / 65535 }
    }
    if (a.action === ACTION.SET && shape) {
      return { entity: toEntity(a.entity), property: Number(a.property), action: Number(a.action), valueKind: COMMAND_VALUE.SCALAR, value: Number(a.value) || 0 }
    }
    return { entity: toEntity(a.entity), property: Number(a.property), action: Number(a.action), valueKind: COMMAND_VALUE.NONE }
  }

  // Держим выбранную Entity триггера внутри допустимого набора его типа.
  useEffect(() => {
    setTriggers((ts) => ts.map((t) => {
      const list = Number(t.kind) === 0 ? eventEntities : Number(t.kind) === 2 ? stateEntities : []
      if (Number(t.kind) === 1 || list.length === 0) return t
      if (list.some((e) => e.id.toString() === t.entity)) return t
      return { ...t, entity: list[0].id.toString() }
    }))
  }, [triggers.length, actions.length]) // eslint-disable-line react-hooks/exhaustive-deps

  const submit = (e) => {
    e.preventDefault()
    if (triggerListInvalid(triggers) || actions.length === 0) return
    const targetId = id != null ? BigInt(id) : nextId(automations)
    store.send(automationPut(targetId, {
      enabled,
      triggers: triggers.map(toTriggerWire),
      actions: actions.map(toActionWire),
      conditions: conditions.map((c) => ({
        entity: toEntity(c.entity),
        property: Number(c.property) || 0,
        op: Number(c.op) || 1,
        value: Number(c.value) || 0,
        value2: Number(c.value2) || 0,
      })),
    }))
    onClose()
  }

  const triggerEventsFor = (t) => {
    if (t.entity === SRC) return []
    const eid = BigInt(t.entity)
    if (eid === ENTITY_SYSTEM) return SYSTEM_EVENT_OPTIONS.map(([v]) => [v, eventName(v)])
    return store.entityEvents(eid).map((v) => [v, eventName(v)])
  }

  return (
    <form className="form-grid" onSubmit={submit}>
      <label className="af-line">Включено <Switch checked={enabled} onChange={setEnabled} /></label>

      <div className="af-section">
        <div className="af-section-head">
          <span>Триггеры (любой запускает правило)</span>
          <button type="button" onClick={() => triggers.length < MAX_TRIGGERS && setTriggers((ts) => [...ts, newTrigger()])} disabled={triggers.length >= MAX_TRIGGERS}>+ Триггер</button>
        </div>
        {triggers.map((t, i) => (
          <div className="af-cond" key={i}>
            <select value={t.kind} onChange={(e) => updateTrigger(i, { kind: Number(e.target.value) })}>
              {TRIGGER_KINDS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {Number(t.kind) === 1 ? (
              <>
                <select value={Math.floor(hhmmToMinutes(t.timeStr) / 60)}
                  onChange={(e) => updateTrigger(i, { timeStr: `${String(Number(e.target.value)).padStart(2, '0')}:${String(hhmmToMinutes(t.timeStr) % 60).padStart(2, '0')}` })}>
                  {Array.from({ length: 24 }, (_, h) => <option key={h} value={h}>{String(h).padStart(2, '0')}</option>)}
                </select>
                <span>:</span>
                <select value={hhmmToMinutes(t.timeStr) % 60}
                  onChange={(e) => updateTrigger(i, { timeStr: `${String(Math.floor(hhmmToMinutes(t.timeStr) / 60)).padStart(2, '0')}:${String(Number(e.target.value)).padStart(2, '0')}` })}>
                  {Array.from({ length: 60 }, (_, m) => <option key={m} value={m}>{String(m).padStart(2, '0')}</option>)}
                </select>
                <span className="days">
                  {WEEKDAY_LABELS.map((d, k) => (
                    <label key={d} className="day">
                      <input type="checkbox" checked={(t.weekdayMask >> k) & 1}
                        onChange={(e) => updateTrigger(i, { weekdayMask: e.target.checked ? (t.weekdayMask | (1 << k)) : (t.weekdayMask & ~(1 << k)) })} />
                      {d}
                    </label>
                  ))}
                </span>
              </>
            ) : Number(t.kind) === 0 ? (
              <>
                <select value={t.entity} onChange={(e) => updateTrigger(i, { entity: e.target.value })}>{entityOptions(eventEntities, false)}</select>
                <select value={t.eventId} onChange={(e) => updateTrigger(i, { eventId: Number(e.target.value) })}>
                  {triggerEventsFor(t).length === 0
                    ? <option value="">(нет событий)</option>
                    : triggerEventsFor(t).map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
              </>
            ) : (
              <>
                <select value={t.entity} onChange={(e) => updateTrigger(i, { entity: e.target.value, property: 0 })}>{entityOptions(stateEntities, false)}</select>
                <select value={t.property || ''} onChange={(e) => updateTrigger(i, { property: Number(e.target.value) })}>
                  <option value="">(свойство)</option>
                  {propsOf(t.entity).map((o) => <option key={o.property} value={o.property}>{propertyName(o.property)}</option>)}
                </select>
                <select value={t.op} onChange={(e) => updateTrigger(i, { op: Number(e.target.value) })}>
                  {TRIGGER_OPS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
                <input className="ep" type="number" step="any" value={t.value} onChange={(e) => updateTrigger(i, { value: e.target.value })} />
                <select value={t.edge} onChange={(e) => updateTrigger(i, { edge: Number(e.target.value) })}>
                  {TRIGGER_EDGES.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
              </>
            )}
            {triggers.length > 1 && <button type="button" className="ghost danger" onClick={() => setTriggers((ts) => ts.filter((_, j) => j !== i))}>✕</button>}
          </div>
        ))}
      </div>

      <div className="af-section">
        <div className="af-section-head">
          <span>Действия (выполняются подряд)</span>
          <button type="button" onClick={() => actions.length < MAX_ACTIONS && setActions((as) => [...as, newAction()])} disabled={actions.length >= MAX_ACTIONS}>+ Действие</button>
        </div>
        {actions.map((a, i) => {
          const opts = actionOptionsFor(a.entity)
          const acts = (opts.find((p) => p.property === a.property) || {}).actions || []
          const shape = valueShapeOf(a.property)
          return (
            <div className="af-cond" key={i}>
              <select value={a.entity} onChange={(e) => updateAction(i, { entity: e.target.value })}>{entityOptions(actionEntities, true, 'то же, что триггер')}</select>
              <select value={a.property} onChange={(e) => pickActionProperty(i, e.target.value)}>
                {opts.map((p) => <option key={p.property} value={p.property}>{propertyName(p.property)}</option>)}
              </select>
              <select value={a.action} onChange={(e) => updateAction(i, { action: Number(e.target.value) })}>
                {acts.map((x) => <option key={x} value={x}>{actionName(x)}</option>)}
              </select>
              {a.action === ACTION.SET && shape === 'color' && (
                <input type="color" value={a.color} onChange={(e) => updateAction(i, { color: e.target.value })} />
              )}
              {a.action === ACTION.SET && shape && shape !== 'color' && (
                <>
                  <input className="ep" type="number" step="any" value={a.value} onChange={(e) => updateAction(i, { value: e.target.value })} />
                  <span className="muted">{shape === 'percent' ? '%' : shape === 'kelvin' ? 'K' : '°'}</span>
                </>
              )}
              {actions.length > 1 && <button type="button" className="ghost danger" onClick={() => setActions((as) => as.filter((_, j) => j !== i))}>✕</button>}
            </div>
          )
        })}
      </div>

      <div className="af-section">
        <div className="af-section-head">
          <span>Условия (все должны выполняться)</span>
          <button type="button" onClick={addCondition} disabled={conditions.length >= MAX_CONDITIONS}>+ Условие</button>
        </div>
        {conditions.length === 0 && <div className="muted">нет — срабатывает всегда</div>}
        {conditions.map((c, i) => {
          const opts = propsOf(c.entity === SRC ? (triggers[0]?.entity ?? SRC) : c.entity)
          return (
            <div className="af-cond" key={i}>
              <select value={c.entity} onChange={(e) => updateCondition(i, { entity: e.target.value, property: 0 })}>
                {entityOptions(stateEntities, true, 'то же, что триггер')}
              </select>
              <select value={c.property || ''} onChange={(e) => updateCondition(i, { property: Number(e.target.value) })}>
                <option value="">(свойство)</option>
                {opts.map((o) => <option key={o.property} value={o.property}>{propertyName(o.property)}</option>)}
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

// Минимальная валидация: у каждого триггера STATE должно быть свойство, EVENT — событие.
function triggerListInvalid(triggers) {
  if (triggers.length === 0) return true
  return triggers.some((t) => (Number(t.kind) === 2 && !t.property) || (Number(t.kind) === 0 && !t.eventId))
}
