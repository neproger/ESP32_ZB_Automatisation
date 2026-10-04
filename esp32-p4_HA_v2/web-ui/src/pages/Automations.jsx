import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { automationPut, automationRemove } from '../proto.js'
import { uidHex, clusterName } from '../zcl.js'
import AutomationForm from '../components/AutomationForm.jsx'

export default function Automations() {
  const s = useStore()
  const [editing, setEditing] = useState(null) // null | 'new' | id

  const list = [...s.automations.values()]
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
        <div className="card" key={'auto:' + key.id}>
          <div className="title">
            <input
              type="checkbox"
              checked={!!record.enabled}
              onChange={(e) => store.send(automationPut(key.id, { ...record, enabled: e.target.checked }))}
            />
            <span>Правило #{key.id}</span>
            <button onClick={() => setEditing(key.id)}>Изменить</button>
            <button onClick={() => store.send(automationRemove(key.id))}>Удалить</button>
          </div>
          <div className="attrs">
            <span className="attr">
              триггер: <b>{nameOf(record.triggerUid)}</b> · команда 0x{record.triggerCmd.toString(16)}
            </span>
            <span className="attr">
              действие: <b>{nameOf(record.actionUid)}</b> · EP{record.actionEp} · {clusterName(record.actionCluster)} · команда 0x{record.actionCmd.toString(16)}
            </span>
          </div>
        </div>
      ))}

      {editing && (
        <AutomationForm
          id={editing === 'new' ? null : editing}
          devices={devices}
          automations={s.automations}
          onClose={() => setEditing(null)}
        />
      )}
    </section>
  )
}
