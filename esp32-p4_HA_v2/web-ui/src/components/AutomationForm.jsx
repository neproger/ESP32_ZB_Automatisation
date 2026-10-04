import { useState } from 'react'
import { store } from '../store.js'
import { automationPut, zbCommand } from '../proto.js'
import { uidHex } from '../zcl.js'
import { ACTION_CLUSTERS, TRIGGER_CMDS, buildActionArgs, decodeActionArgs } from '../automation.js'

function nextId(automations) {
  let max = 0n
  for (const { key } of automations.values()) if (key.id > max) max = key.id
  return max + 1n
}

export default function AutomationForm({ id, devices, automations, onClose }) {
  const existing = id != null ? automations.get('auto:' + id)?.record : null
  const params = existing ? decodeActionArgs(existing.actionCluster, existing.actionCmd, existing.actionArgs) : {}

  const [enabled, setEnabled] = useState(existing ? !!existing.enabled : true)
  const [triggerUid, setTriggerUid] = useState(existing ? existing.triggerUid.toString() : '0')
  const [triggerCmd, setTriggerCmd] = useState(existing ? String(existing.triggerCmd) : '2')
  const [actionUid, setActionUid] = useState(existing ? existing.actionUid.toString() : '0')
  const [actionEp, setActionEp] = useState(existing ? existing.actionEp : 1)
  const [actionCluster, setActionCluster] = useState(existing ? existing.actionCluster : 0x0006)
  const [actionCmd, setActionCmd] = useState(existing ? existing.actionCmd : 2)
  const [level, setLevel] = useState(params.level ?? 128)
  const [transitionMs, setTransitionMs] = useState(params.transitionMs ?? 0)
  const [kelvin, setKelvin] = useState(params.kelvin ?? 3000)
  const [colorHex, setColorHex] = useState(params.colorHex ?? '#ffcc88')

  const clusterDef = ACTION_CLUSTERS.find((c) => c.id === actionCluster) || ACTION_CLUSTERS[0]
  const params0 = { level, transitionMs, kelvin, colorHex }
  const args = buildActionArgs(actionCluster, actionCmd, params0)

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

  const submit = (e) => {
    e.preventDefault()
    const targetId = id != null ? BigInt(id) : nextId(automations)
    store.send(
      automationPut(targetId, {
        enabled,
        triggerUid: BigInt(triggerUid),
        triggerCmd: Number(triggerCmd),
        actionUid: BigInt(actionUid),
        actionEp: Number(actionEp),
        actionCluster,
        actionCmd,
        actionArgs: args,
      }),
    )
    onClose()
  }

  const test = () => {
    const uid = BigInt(actionUid) !== 0n ? BigInt(actionUid) : BigInt(triggerUid)
    if (uid === 0n) return
    store.send(zbCommand({ uid, ep: Number(actionEp), cluster: actionCluster, command: actionCmd, args }))
  }

  return (
    <form className="auto-form" onSubmit={submit}>
      <label className="af-line"><input type="checkbox" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} /> Включено</label>

      <label className="af-line">Триггер: <select value={triggerUid} onChange={(e) => setTriggerUid(e.target.value)}>{devOptions('любое устройство')}</select>
        <select value={triggerCmd} onChange={(e) => setTriggerCmd(e.target.value)}>{TRIGGER_CMDS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}</select>
      </label>

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

      <div className="af-line">
        <button type="submit">Сохранить</button>
        <button type="button" onClick={test}>Проверить</button>
        <button type="button" onClick={onClose}>Отмена</button>
      </div>
    </form>
  )
}
