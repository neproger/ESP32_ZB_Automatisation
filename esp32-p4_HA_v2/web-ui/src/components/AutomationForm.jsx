import { useState } from 'react'
import { store } from '../store.js'
import { automationPut, zbCommand } from '../proto.js'
import { uidHex, clusterName, attrName, describeAttr } from '../zcl.js'
import {
  ACTION_CLUSTERS, TRIGGER_CMDS, TRIGGER_KINDS, TRIGGER_OPS, TRIGGER_EDGES, WEEKDAY_LABELS,
  CONDITION_OPS, buildActionArgs, decodeActionArgs, minutesToHHMM, hhmmToMinutes,
} from '../automation.js'
import { SYSTEM_DEVICE_UID, SYSTEM_EVENTS } from '../system.js'

const MAX_CONDITIONS = 4

function nextId(automations) {
  let max = 0n
  for (const { key } of automations.values()) if (key.id > max) max = key.id
  return max + 1n
}

// Атрибуты, по которым у устройства уже есть состояние (ключи Entity Store).
function statesFor(uid) {
  const out = []
  if (uid === 0n) return out
  for (const st of store.states.values()) if (st.key.uid === uid) out.push(st.key)
  out.sort((a, b) => a.cluster - b.cluster || a.attr - b.attr || a.ep - b.ep)
  return out
}

export default function AutomationForm({ id, devices, automations, onClose }) {
  const existing = id != null ? automations.get('auto:' + id)?.record : null
  const params = existing ? decodeActionArgs(existing.actionCluster, existing.actionCmd, existing.actionArgs) : {}

  const [enabled, setEnabled] = useState(existing ? !!existing.enabled : true)
  const [triggerKind, setTriggerKind] = useState(existing ? existing.triggerKind : 0)
  const [triggerUid, setTriggerUid] = useState(existing ? existing.triggerUid.toString() : '0')
  const [triggerCmd, setTriggerCmd] = useState(existing ? String(existing.triggerCmd) : '2')
  const [timeStr, setTimeStr] = useState(minutesToHHMM(existing ? existing.triggerMinutesOfDay : 420))
  const [weekdayMask, setWeekdayMask] = useState(existing ? existing.triggerWeekdayMask : 0x7f)
  const isState = (e) => e && e.triggerKind === 2
  const [stateValue, setStateValue] = useState(isState(existing) ? existing.triggerValue : 0)
  const [stateEp, setStateEp] = useState(isState(existing) ? existing.triggerEp : 0)
  const [stateCluster, setStateCluster] = useState(isState(existing) ? existing.triggerCluster : 0)
  const [stateAttr, setStateAttr] = useState(isState(existing) ? existing.triggerAttr : 0)
  const [stateOp, setStateOp] = useState(isState(existing) ? existing.triggerOp : 1)
  const [stateEdge, setStateEdge] = useState(isState(existing) ? existing.triggerEdge : 1)
  const [actionUid, setActionUid] = useState(existing ? existing.actionUid.toString() : '0')
  const [actionEp, setActionEp] = useState(existing ? existing.actionEp : 1)
  const [actionCluster, setActionCluster] = useState(existing ? existing.actionCluster : 0x0006)
  const [actionCmd, setActionCmd] = useState(existing ? existing.actionCmd : 2)
  const [level, setLevel] = useState(params.level ?? 128)
  const [transitionMs, setTransitionMs] = useState(params.transitionMs ?? 0)
  const [kelvin, setKelvin] = useState(params.kelvin ?? 3000)
  const [colorHex, setColorHex] = useState(params.colorHex ?? '#ffcc88')
  const [conditions, setConditions] = useState(existing ? existing.conditions.map((c) => ({ ...c })) : [])

  const clusterDef = ACTION_CLUSTERS.find((c) => c.id === actionCluster) || ACTION_CLUSTERS[0]
  const params0 = { level, transitionMs, kelvin, colorHex }
  const args = buildActionArgs(actionCluster, actionCmd, params0)

  // Для системного девайса триггер — его события (тики времени), иначе — ZCL-команды.
  const isSystemTrigger = triggerUid !== '0' && BigInt(triggerUid) === SYSTEM_DEVICE_UID
  const triggerOptions = isSystemTrigger
    ? Object.keys(SYSTEM_EVENTS).map(Number).sort((a, b) => a - b).map((id) => [id, SYSTEM_EVENTS[id]])
    : TRIGGER_CMDS

  const stateAttrs = statesFor(triggerUid !== '0' ? BigInt(triggerUid) : 0n)
  const isBoolState = stateCluster === 0x0006 && stateAttr === 0x0000

  const devOptions = (anyLabel) => [
    <option key="0" value="0">{anyLabel}</option>,
    ...devices.map((d) => (
      <option key={d.key.uid.toString()} value={d.key.uid.toString()}>
        {d.record.name || d.record.model || uidHex(d.key.uid)}
      </option>
    )),
  ]

  const pickCluster = (v) => {
    const c = ACTION_CLUSTERS.find((x) => x.id === Number(v)) || ACTION_CLUSTERS[0]
    setActionCluster(c.id)
    setActionCmd(c.cmds[0][0])
  }

  const updateCondition = (i, patch) => setConditions((cs) => cs.map((c, j) => (j === i ? { ...c, ...patch } : c)))
  const addCondition = () => {
    if (conditions.length >= MAX_CONDITIONS) return
    setConditions((cs) => [...cs, { deviceUid: 0n, cluster: 0x0006, attr: 0, ep: 0, op: 1, value: 1, value2: 0 }])
  }

  const submit = (e) => {
    e.preventDefault()
    const targetId = id != null ? BigInt(id) : nextId(automations)
    store.send(
      automationPut(targetId, {
        enabled,
        triggerKind,
        triggerUid: BigInt(triggerUid),
        triggerCmd: Number(triggerCmd),
        triggerMinutesOfDay: Number(triggerKind) === 1 ? hhmmToMinutes(timeStr) : 0,
        triggerWeekdayMask: Number(triggerKind) === 1 ? weekdayMask : 0,
        triggerValue: Number(triggerKind) === 2 ? (Number(stateValue) || 0) : 0,
        triggerEp: Number(triggerKind) === 2 ? stateEp : 0,
        triggerCluster: Number(triggerKind) === 2 ? stateCluster : 0,
        triggerAttr: Number(triggerKind) === 2 ? stateAttr : 0,
        triggerOp: Number(triggerKind) === 2 ? stateOp : 0,
        triggerEdge: Number(triggerKind) === 2 ? stateEdge : 0,
        actionUid: BigInt(actionUid),
        actionEp: Number(actionEp),
        actionCluster,
        actionCmd,
        actionArgs: args,
        conditions: conditions.map((c) => ({
          deviceUid: BigInt(c.deviceUid || 0),
          cluster: c.cluster,
          attr: c.attr,
          ep: c.ep,
          op: c.op,
          value: Number(c.value) || 0,
          value2: Number(c.value2) || 0,
        })),
      }),
    )
    onClose()
  }

  const test = () => {
    const uid = BigInt(actionUid) !== 0n ? BigInt(actionUid) : BigInt(triggerUid)
    if (uid === 0n) return
    store.send(zbCommand({ uid, ep: Number(actionEp), cluster: actionCluster, command: actionCmd, args }))
  }

  const condDeviceFor = (c) => {
    if (c.deviceUid && c.deviceUid !== 0n) return c.deviceUid
    return triggerUid !== '0' ? BigInt(triggerUid) : 0n
  }
  // Тип берём из живого состояния: bool (0x10) показываем как Вкл/Выкл.
  const zclTypeFor = (c) => {
    const uid = condDeviceFor(c)
    if (uid === 0n) return undefined
    return store.states.get(`st:${uid}:${c.ep}:${c.cluster}:${c.attr}`)?.record.zclType
  }
  const isBoolCond = (c) => {
    const t = zclTypeFor(c)
    return t != null ? t === 0x10 : c.cluster === 0x0006 && c.attr === 0x0000
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
            <span className="time-pick">
              <select value={Math.floor(hhmmToMinutes(timeStr) / 60)}
                onChange={(e) => setTimeStr(`${String(Number(e.target.value)).padStart(2, '0')}:${String(hhmmToMinutes(timeStr) % 60).padStart(2, '0')}`)}>
                {Array.from({ length: 24 }, (_, h) => <option key={h} value={h}>{String(h).padStart(2, '0')}</option>)}
              </select>
              <span>:</span>
              <select value={hhmmToMinutes(timeStr) % 60}
                onChange={(e) => setTimeStr(`${String(Math.floor(hhmmToMinutes(timeStr) / 60)).padStart(2, '0')}:${String(Number(e.target.value)).padStart(2, '0')}`)}>
                {Array.from({ length: 60 }, (_, m) => <option key={m} value={m}>{String(m).padStart(2, '0')}</option>)}
              </select>
            </span>
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
        ) : Number(triggerKind) === 2 ? (
          <>
            <select value={triggerUid} onChange={(e) => {
              setTriggerUid(e.target.value)
              setStateCluster(0); setStateAttr(0); setStateEp(0)
            }}>
              <option value="0">выберите устройство</option>
              {devices.map((d) => (
                <option key={d.key.uid.toString()} value={d.key.uid.toString()}>
                  {d.record.name || d.record.model || uidHex(d.key.uid)}
                </option>
              ))}
            </select>
            <select value={stateCluster || stateAttr ? `${stateEp}:${stateCluster}:${stateAttr}` : ''}
              onChange={(e) => {
                const [ep, cluster, attr] = e.target.value.split(':').map(Number)
                setStateEp(ep); setStateCluster(cluster); setStateAttr(attr); setStateValue(1)
              }}>
              <option value="">(атрибут)</option>
              {stateAttrs.map((k) => (
                <option key={`${k.ep}:${k.cluster}:${k.attr}`} value={`${k.ep}:${k.cluster}:${k.attr}`}>
                  {clusterName(k.cluster)} · {attrName(k.cluster, k.attr)} (EP{k.ep})
                </option>
              ))}
            </select>
            <select value={stateOp} onChange={(e) => setStateOp(Number(e.target.value))}>
              {TRIGGER_OPS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {isBoolState ? (
              <select value={stateValue ? 1 : 0} onChange={(e) => setStateValue(Number(e.target.value))}>
                <option value={1}>Вкл</option>
                <option value={0}>Выкл</option>
              </select>
            ) : (
              <>
                <input className="ep" type="number" step="any" value={stateValue} onChange={(e) => setStateValue(e.target.value)} />
                <span className="muted">{describeAttr(stateCluster, stateAttr)?.unit || ''}</span>
              </>
            )}
            <select value={stateEdge} onChange={(e) => setStateEdge(Number(e.target.value))}>
              {TRIGGER_EDGES.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </>
        ) : (
          <>
            <select value={triggerUid} onChange={(e) => {
              const u = e.target.value
              setTriggerUid(u)
              setTriggerCmd(u !== '0' && BigInt(u) === SYSTEM_DEVICE_UID ? '1' : '2')
            }}>{devOptions('любое устройство')}</select>
            <select value={triggerCmd} onChange={(e) => setTriggerCmd(e.target.value)}>{triggerOptions.map(([v, l]) => <option key={v} value={v}>{l}</option>)}</select>
          </>
        )}
      </div>

      <label className="af-line">Действие: <select value={actionUid} onChange={(e) => setActionUid(e.target.value)}>{devOptions('то же, что триггер')}</select>
        EP <input className="ep" type="number" min="0" max="255" value={actionEp} onChange={(e) => setActionEp(e.target.value)} />
        <select value={actionCluster} onChange={(e) => pickCluster(e.target.value)}>{ACTION_CLUSTERS.map((c) => <option key={c.id} value={c.id}>{c.name}</option>)}</select>
        <select value={actionCmd} onChange={(e) => setActionCmd(Number(e.target.value))}>{clusterDef.cmds.map(([v, l]) => <option key={v} value={v}>{l}</option>)}</select>
      </label>

      {actionCluster === 0x0008 && (
        <label className="af-line">Уровень: <input type="range" min="0" max="254" value={level} onChange={(e) => setLevel(Number(e.target.value))} /> {level}
          <span className="muted">transition, мс</span><input className="ep" type="number" min="0" value={transitionMs} onChange={(e) => setTransitionMs(Number(e.target.value))} />
        </label>
      )}

      {actionCluster === 0x0300 && actionCmd === 1 && (
        <label className="af-line">Темп. цвета: <input type="range" min="2000" max="6500" step="100" value={kelvin} onChange={(e) => setKelvin(Number(e.target.value))} /> {kelvin} K
        </label>
      )}

      {actionCluster === 0x0300 && actionCmd === 0 && (
        <label className="af-line">Цвет: <input type="color" value={colorHex} onChange={(e) => setColorHex(e.target.value)} />
        </label>
      )}

      <div className="af-section">
        <div className="af-section-head">
          <span>Условия (все должны выполняться)</span>
          <button type="button" onClick={addCondition} disabled={conditions.length >= MAX_CONDITIONS}>+ Условие</button>
        </div>
        {conditions.length === 0 && <div className="muted">нет — срабатывает всегда</div>}
        {conditions.map((c, i) => {
          const opts = statesFor(condDeviceFor(c))
          const attrValue = c.cluster || c.attr ? `${c.ep}:${c.cluster}:${c.attr}` : ''
          return (
            <div className="af-cond" key={i}>
              <select value={c.deviceUid.toString()} onChange={(e) => updateCondition(i, { deviceUid: BigInt(e.target.value), cluster: 0, attr: 0, ep: 0 })}>
                {devOptions('то же, что триггер')}
              </select>
              <select value={attrValue} onChange={(e) => {
                const [ep, cluster, attr] = e.target.value.split(':').map(Number)
                updateCondition(i, { ep, cluster, attr, value: 1 })
              }}>
                <option value="">(атрибут)</option>
                {opts.map((k) => (
                  <option key={`${k.ep}:${k.cluster}:${k.attr}`} value={`${k.ep}:${k.cluster}:${k.attr}`}>
                    {clusterName(k.cluster)} · {attrName(k.cluster, k.attr)} (EP{k.ep})
                  </option>
                ))}
              </select>
              <select value={c.op} onChange={(e) => updateCondition(i, { op: Number(e.target.value) })}>
                {CONDITION_OPS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
              {c.op === 8 ? (
                <>
                  <input className="ep" type="number" step="any" value={c.value} onChange={(e) => updateCondition(i, { value: e.target.value })} />
                  <span className="muted">…</span>
                  <input className="ep" type="number" step="any" value={c.value2 ?? 0} onChange={(e) => updateCondition(i, { value2: e.target.value })} />
                  <span className="muted">{describeAttr(c.cluster, c.attr)?.unit || ''}</span>
                </>
              ) : isBoolCond(c) ? (
                <select value={c.value ? 1 : 0} onChange={(e) => updateCondition(i, { value: Number(e.target.value) })}>
                  <option value={1}>Вкл</option>
                  <option value={0}>Выкл</option>
                </select>
              ) : (
                <>
                  <input className="ep" type="number" step="any" value={c.value} onChange={(e) => updateCondition(i, { value: e.target.value })} />
                  <span className="muted">{describeAttr(c.cluster, c.attr)?.unit || ''}</span>
                </>
              )}
              <button type="button" className="ghost danger" onClick={() => setConditions((cs) => cs.filter((_, j) => j !== i))}>✕</button>
            </div>
          )
        })}
      </div>

      <div className="af-line">
        <button type="submit">Сохранить</button>
        <button type="button" onClick={test}>Проверить</button>
        <button type="button" onClick={onClose}>Отмена</button>
      </div>
    </form>
  )
}
