import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { automationPut, automationRemove } from '../proto.js'
import { uidHex, clusterName } from '../zcl.js'
import { describeActionArgs, describeCondition, describeStateTrigger, TRIGGER_CMDS, ACTION_CLUSTERS, minutesToHHMM, maskToDays } from '../automation.js'
import AutomationForm from '../components/AutomationForm.jsx'
import Modal from '../components/Modal.jsx'
import { SYSTEM_DEVICE_UID, SYSTEM_EVENTS } from '../system.js'

function triggerCmdName(id) {
  const e = TRIGGER_CMDS.find(([v]) => v === id)
  return e ? e[1] : '0x' + id.toString(16)
}
function triggerName(uid, cmd) {
  if (uid === SYSTEM_DEVICE_UID) return SYSTEM_EVENTS[cmd] || '0x' + cmd.toString(16)
  return triggerCmdName(cmd)
}
function actionCmdName(cluster, cmd) {
  const c = ACTION_CLUSTERS.find((x) => x.id === cluster)
  const e = c && c.cmds.find(([v]) => v === cmd)
  return e ? e[1] : '0x' + cmd.toString(16)
}

export default function Automations() {
  const s = useStore()
  const [editing, setEditing] = useState(null) // null | 'new' | id

  const list = [...s.automations.values()].sort((a, b) => (a.key.id < b.key.id ? -1 : a.key.id > b.key.id ? 1 : 0))
  const devices = [...s.devices.values()]
  const nameOf = (uid) => {
    if (uid === 0n) return '—'
    const d = devices.find((x) => x.key.uid === uid)
    return d ? d.record.name || d.record.model || uidHex(uid) : uidHex(uid)
  }

  return (
    <section>
      <h2>
        Автоматизации{' '}
        <button onClick={() => setEditing('new')}>Добавить</button>
      </h2>

      {list.length === 0 && <p className="muted pad">правил пока нет</p>}

      {list.map(({ key, record }) => (
        <div className={`card${record.enabled ? '' : ' off'}`} key={'auto:' + key.id}>
          <div className="card-head">
            <label className="switch">
              <input
                type="checkbox"
                checked={!!record.enabled}
                onChange={(e) => store.send(automationPut(key.id, { ...record, enabled: e.target.checked }))}
              />
              <span />
            </label>
            <span className="rule-id">Правило #{key.id}</span>
            <span className="spacer" />
            <button className="ghost" onClick={() => setEditing(key.id)}>Изменить</button>
            <button className="ghost danger" onClick={() => store.send(automationRemove(key.id))}>Удалить</button>
          </div>
          <div className="flow">
            <span className="node trigger">
              {record.triggerKind === 2
                ? `Состояние ${describeStateTrigger(record, nameOf)}`
                : record.triggerKind === 1
                  ? `Время ${minutesToHHMM(record.triggerMinutesOfDay)} · ${maskToDays(record.triggerWeekdayMask) || '—'}`
                  : `${nameOf(record.triggerUid)} · ${triggerName(record.triggerUid, record.triggerCmd)}`}
            </span>
            <span className="arrow">→</span>
            <span className="node action">
              {nameOf(record.actionUid)} · EP{record.actionEp} · {clusterName(record.actionCluster)} · {actionCmdName(record.actionCluster, record.actionCmd)}
              {describeActionArgs(record.actionCluster, record.actionCmd, record.actionArgs)
                ? ' · ' + describeActionArgs(record.actionCluster, record.actionCmd, record.actionArgs)
                : ''}
            </span>
          </div>
          {record.conditions?.length > 0 && (
            <div className="conds">
              <span className="muted">если</span>
              {record.conditions.map((c, i) => (
                <span className="chip" key={i}>{describeCondition(c, nameOf)}</span>
              ))}
            </div>
          )}
        </div>
      ))}

      {editing && (
        <Modal
          title={editing === 'new' ? 'Новая автоматизация' : `Правило #${editing}`}
          onClose={() => setEditing(null)}
          wide
        >
          <AutomationForm
            id={editing === 'new' ? null : editing}
            devices={devices}
            automations={s.automations}
            onClose={() => setEditing(null)}
          />
        </Modal>
      )}
    </section>
  )
}
