import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { automationPut, ENTITY_SOURCE } from '../proto.js'
import {
  TRIGGER_KINDS, TRIGGER_OPS, TRIGGER_EDGES, CONDITION_OPS, WEEKDAY_LABELS,
  SYSTEM_EVENT_OPTIONS, ACTION_PROPERTIES, minutesToHHMM, hhmmToMinutes,
} from '../automation.js'
import { PROPERTY, ACTION, COMMAND_VALUE, propertyName, actionName, eventName } from '../semantics.js'
import { rgbHexToXy } from '../color.js'
import { ENTITY_SYSTEM } from '../system.js'

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

export default function AutomationForm({ id, automations, onClose }) {
  const s = useStore()
  const existing = id != null ? s.semAutomations.get(String(id))?.rule : null
  const ents = [...s.semEntities.values()]
  const hasSystem = ents.some((e) => e.name === 'system' || e.name === 'Система') || true

  if (ents.length === 0 && !existing) {
    return (
      <div className="form-grid">
        <p className="muted">нет сущностей — сначала добавьте устройства</p>
        <button type="button" onClick={onClose}>Закрыть</button>
      </div>
    )
  }

  // Опции Entity, сгруппированные по устройству (имя устройства видно, даже если у Entity
  // имени нет). Внутри группы — имя Entity, иначе короткий id + подсказка по capability.
  const entityLabel = (e) => {
    if (e.name) return e.name
    const caps = store.capabilities(e.id)
    const hint = caps.length > 0 ? ` · ${propertyName(caps[0].property)}` : ''
    return `#${e.id.toString().slice(-4)}${hint}`
  }
  const entityOptions = (includeSource, sourceLabel) => {
    const nodes = []
    if (includeSource) nodes.push(<option key="src" value={SRC}>{sourceLabel}</option>)
    const groups = new Map()
    for (const e of ents) {
      const dk = e.deviceId?.toString() ?? ''
      if (!groups.has(dk)) groups.set(dk, [])
      groups.get(dk).push(e)
    }
    for (const [dk, list] of groups) {
      const dn = s.semDevices.get(dk)?.name || (dk ? 'устройство ' + dk.slice(-6) : 'без устройства')
      nodes.push(
        <optgroup key={'g' + dk} label={dn}>
          {list.map((e) => (
            <option key={e.id.toString()} value={e.id.toString()}>{entityLabel(e)}</option>
          ))}
        </optgroup>,
      )
    }
    return nodes
  }

  const toEntity = (v) => (v === SRC ? ENTITY_SOURCE : BigInt(v))
  const fromEntity = (e) => (e == null || BigInt(e) === ENTITY_SOURCE ? SRC : BigInt(e).toString())

  const [enabled, setEnabled] = useState(existing ? !!existing.enabled : true)
  const [triggerKind, setTriggerKind] = useState(existing ? existing.trigger.kind : 1)
  const [triggerEntity, setTriggerEntity] = useState(existing?.trigger.entity != null ? fromEntity(existing.trigger.entity) : (ents[0]?.id.toString() ?? SRC))
  const [eventId, setEventId] = useState(existing?.trigger.eventId ?? SYSTEM_EVENT_OPTIONS[0][0])
  const [timeStr, setTimeStr] = useState(minutesToHHMM(existing?.trigger.minutesOfDay ?? 420))
  const [weekdayMask, setWeekdayMask] = useState(existing?.trigger.weekdayMask ?? 0x7f)
  const [stateProperty, setStateProperty] = useState(existing?.trigger.property ?? 0)
  const [stateOp, setStateOp] = useState(existing?.trigger.op ?? 1)
  const [stateEdge, setStateEdge] = useState(existing?.trigger.edge ?? 1)
  const [stateValue, setStateValue] = useState(existing?.trigger.value ?? 0)

  const [conditions, setConditions] = useState(
    existing ? existing.conditions.map((c) => ({ ...c, entity: fromEntity(c.entity) })) : [],
  )

  const [actionEntity, setActionEntity] = useState(existing?.action.entity != null ? fromEntity(existing.action.entity) : SRC)
  const [actionProperty, setActionProperty] = useState(existing?.action.property ?? PROPERTY.POWER)
  const [actionAction, setActionAction] = useState(existing?.action.action ?? ACTION.ON)
  const [actionValue, setActionValue] = useState(existing?.action.value ?? 70)
  const [actionColor, setActionColor] = useState('#ffcc88')

  const trigProps = propsOf(triggerEntity)

  // События триггера — только те, что Entity заявила в capabilities (system — свои).
  const triggerEntityId = triggerEntity === SRC ? null : BigInt(triggerEntity)
  const triggerEvents = triggerEntityId == null
    ? []
    : triggerEntityId === ENTITY_SYSTEM
      ? SYSTEM_EVENT_OPTIONS.map(([v]) => [v, eventName(v)])
      : store.entityEvents(triggerEntityId).map((v) => [v, eventName(v)])

  // Действия — из capabilities выбранной Entity; ACTION_PROPERTIES — только fallback, пока
  // capabilities неизвестны (UI-сентинел «то же, что триггер» не резолвится).
  const actionCaps = actionEntity === SRC ? null : propsOf(actionEntity)
  const actionOptions = actionCaps ?? ACTION_PROPERTIES.map((p) => ({ property: p.id, actions: p.actions }))
  const actionActions = (actionOptions.find((p) => p.property === actionProperty) || {}).actions || []
  const actionValueShape = (ACTION_PROPERTIES.find((p) => p.id === actionProperty) || {}).value
  const pickProperty = (v) => {
    const property = Number(v)
    setActionProperty(property)
    const opt = actionOptions.find((p) => p.property === property)
    setActionAction(opt?.actions?.[0] ?? ACTION.SET)
  }

  const updateCondition = (i, patch) => setConditions((cs) => cs.map((c, j) => (j === i ? { ...c, ...patch } : c)))
  const addCondition = () => {
    if (conditions.length >= MAX_CONDITIONS) return
    setConditions((cs) => [...cs, { entity: SRC, property: PROPERTY.POWER, op: 1, value: 1, value2: 0 }])
  }

  const submit = (e) => {
    e.preventDefault()
    const targetId = id != null ? BigInt(id) : nextId(automations)
    const trigger =
      Number(triggerKind) === 1
        ? { kind: 1, minutesOfDay: hhmmToMinutes(timeStr), weekdayMask }
        : Number(triggerKind) === 0
          ? { kind: 0, entity: toEntity(triggerEntity), eventId: Number(eventId) }
          : { kind: 2, entity: toEntity(triggerEntity), property: Number(stateProperty), op: Number(stateOp), edge: Number(stateEdge), value: Number(stateValue) || 0 }

    let action
    if (actionAction === ACTION.SET && actionValueShape === 'color') {
      const { x, y } = rgbHexToXy(actionColor)
      action = { entity: toEntity(actionEntity), property: actionProperty, action: actionAction, valueKind: COMMAND_VALUE.XY, x: x / 65535, y: y / 65535 }
    } else if (actionAction === ACTION.SET) {
      action = { entity: toEntity(actionEntity), property: actionProperty, action: actionAction, valueKind: COMMAND_VALUE.SCALAR, value: Number(actionValue) || 0 }
    } else {
      action = { entity: toEntity(actionEntity), property: actionProperty, action: actionAction, valueKind: COMMAND_VALUE.NONE }
    }

    store.send(
      automationPut(targetId, {
        enabled,
        trigger,
        conditions: conditions.map((c) => ({
          entity: toEntity(c.entity),
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
            <select value={triggerEntity} onChange={(e) => setTriggerEntity(e.target.value)}>
              {entityOptions(false)}
            </select>
            <select value={eventId} onChange={(e) => setEventId(Number(e.target.value))}>
              {triggerEvents.length === 0
                ? <option value="">(нет событий)</option>
                : triggerEvents.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </>
        ) : (
          <>
            <select value={triggerEntity} onChange={(e) => { setTriggerEntity(e.target.value); setStateProperty(0) }}>
              {entityOptions(false)}
            </select>
            <select value={stateProperty || ''} onChange={(e) => setStateProperty(Number(e.target.value))}>
              <option value="">(свойство)</option>
              {trigProps.map((o) => <option key={o.property} value={o.property}>{propertyName(o.property)}</option>)}
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
        <select value={actionEntity} onChange={(e) => setActionEntity(e.target.value)}>
          {entityOptions(true, 'то же, что триггер')}
        </select>
        <select value={actionProperty} onChange={(e) => pickProperty(e.target.value)}>
          {actionOptions.map((p) => <option key={p.property} value={p.property}>{propertyName(p.property)}</option>)}
        </select>
        <select value={actionAction} onChange={(e) => setActionAction(Number(e.target.value))}>
          {actionActions.map((a) => <option key={a} value={a}>{actionName(a)}</option>)}
        </select>
      </label>

      {actionAction === ACTION.SET && actionValueShape === 'color' && (
        <label className="af-line">Цвет: <input type="color" value={actionColor} onChange={(e) => setActionColor(e.target.value)} /></label>
      )}
      {actionAction === ACTION.SET && actionValueShape && actionValueShape !== 'color' && (
        <label className="af-line">Значение:
          <input className="ep" type="number" step="any" value={actionValue} onChange={(e) => setActionValue(e.target.value)} />
          <span className="muted">{actionValueShape === 'percent' ? '%' : actionValueShape === 'kelvin' ? 'K' : '°'}</span>
        </label>
      )}

      <div className="af-section">
        <div className="af-section-head">
          <span>Условия (все должны выполняться)</span>
          <button type="button" onClick={addCondition} disabled={conditions.length >= MAX_CONDITIONS}>+ Условие</button>
        </div>
        {conditions.length === 0 && <div className="muted">нет — срабатывает всегда</div>}
        {conditions.map((c, i) => {
          const opts = propsOf(c.entity === SRC ? triggerEntity : c.entity)
          return (
            <div className="af-cond" key={i}>
              <select value={c.entity} onChange={(e) => updateCondition(i, { entity: e.target.value, property: 0 })}>
                {entityOptions(true, 'то же, что триггер')}
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
