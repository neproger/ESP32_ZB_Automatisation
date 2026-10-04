import { useState } from 'react'
import { store } from '../store.js'
import { automationPut } from '../proto.js'
import { uidHex, CLUSTER_ONOFF } from '../zcl.js'

const TRIGGER_CMDS = [
  ['Toggle', 2],
  ['Вкл', 1],
  ['Выкл', 0],
]
const ACTION_CLUSTERS = [
  ['On/Off', 6],
  ['Level', 8],
]
const ACTION_CMDS = {
  6: [['Toggle', 2], ['Вкл', 1], ['Выкл', 0]],
  8: [['Move to level', 0]],
}

function nextId(automations) {
  let max = 0n
  for (const { key } of automations.values()) if (key.id > max) max = key.id
  return max + 1n
}

export default function AutomationForm({ id, devices, automations, onClose }) {
  const existing = id != null ? automations.get('auto:' + id)?.record : null
  const [enabled, setEnabled] = useState(existing ? !!existing.enabled : true)
  const [triggerUid, setTriggerUid] = useState(existing ? existing.triggerUid.toString() : '0')
  const [triggerCmd, setTriggerCmd] = useState(existing ? String(existing.triggerCmd) : '2')
  const [actionUid, setActionUid] = useState(existing ? existing.actionUid.toString() : '0')
  const [actionEp, setActionEp] = useState(existing ? existing.actionEp : 1)
  const [actionCluster, setActionCluster] = useState(existing ? String(existing.actionCluster) : String(CLUSTER_ONOFF))
  const [actionCmd, setActionCmd] = useState(existing ? String(existing.actionCmd) : '2')

  const devOptions = (anyLabel) => [
    <option key="0" value="0">{anyLabel}</option>,
    ...devices.map((d) => (
      <option key={d.key.uid.toString()} value={d.key.uid.toString()}>
        {d.record.name || d.record.model || uidHex(d.key.uid)}
      </option>
    )),
  ]

  const cmds = ACTION_CMDS[Number(actionCluster)] || ACTION_CMDS[6]

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
        actionCluster: Number(actionCluster),
        actionCmd: Number(actionCmd),
        actionArgs: [],
      }),
    )
    onClose()
  }

  return (
    <form className="auto-form" onSubmit={submit}>
      <label className="af-line"><input type="checkbox" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} /> Включено</label>
      <label className="af-line">Триггер, устройство: <select value={triggerUid} onChange={(e) => setTriggerUid(e.target.value)}>{devOptions('любое устройство')}</select></label>
      <label className="af-line">команда: <select value={triggerCmd} onChange={(e) => setTriggerCmd(e.target.value)}>{TRIGGER_CMDS.map(([l, v]) => <option key={v} value={v}>{l}</option>)}</select></label>
      <label className="af-line">Действие, устройство: <select value={actionUid} onChange={(e) => setActionUid(e.target.value)}>{devOptions('то же, что триггер')}</select></label>
      <label className="af-line">endpoint: <input type="number" min="0" max="255" value={actionEp} onChange={(e) => setActionEp(e.target.value)} /></label>
      <label className="af-line">кластер: <select value={actionCluster} onChange={(e) => { setActionCluster(e.target.value); const list = ACTION_CMDS[Number(e.target.value)] || ACTION_CMDS[6]; setActionCmd(String(list[0][1])) }}>{ACTION_CLUSTERS.map(([l, v]) => <option key={v} value={v}>{l}</option>)}</select></label>
      <label className="af-line">команда: <select value={actionCmd} onChange={(e) => setActionCmd(e.target.value)}>{cmds.map(([l, v]) => <option key={v} value={v}>{l}</option>)}</select></label>
      <div className="af-line"><button type="submit">Сохранить</button><button type="button" onClick={onClose}>Отмена</button></div>
    </form>
  )
}
