import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { automationRemove } from '../proto.js'
import { uidHex } from '../zcl.js'
import { describeTrigger, describeCondition, describeAction } from '../automation.js'
import AutomationForm from '../components/AutomationForm.jsx'
import Modal from '../components/Modal.jsx'

export default function Automations() {
  const s = useStore()
  const [editing, setEditing] = useState(null) // null | 'new' | id-string
  const devices = [...s.devices.values()]
  const nameOf = (uid) => {
    if (!uid || uid === 0n) return '—'
    const d = devices.find((x) => x.key.uid === uid)
    return d ? d.record.name || d.record.model || uidHex(uid) : uidHex(uid)
  }
  const list = [...s.semAutomations.values()].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))

  return (
    <section>
      <h2>
        Автоматизации{' '}
        <button onClick={() => setEditing('new')}>Добавить</button>
      </h2>

      {list.length === 0 && <p className="muted pad">правил пока нет</p>}

      {list.map((a) => (
        <div className={`card${a.representable && a.rule.enabled ? '' : ' off'}`} key={a.id.toString()}>
          <div className="card-head">
            <span className="rule-id">Правило #{a.id.toString()}</span>
            <span className="spacer" />
            {a.representable && <button className="ghost" onClick={() => setEditing(a.id.toString())}>Изменить</button>}
            <button className="ghost danger" onClick={() => store.send(automationRemove(a.id))}>Удалить</button>
          </div>
          {a.representable ? (
            <>
              <div className="flow">
                <span className="node trigger">{describeTrigger(a.rule.trigger, nameOf)}</span>
                <span className="arrow">→</span>
                <span className="node action">{describeAction(a.rule.action, nameOf)}</span>
              </div>
              {a.rule.conditions?.length > 0 && (
                <div className="conds">
                  <span className="muted">если</span>
                  {a.rule.conditions.map((c, i) => <span className="chip" key={i}>{describeCondition(c, nameOf)}</span>)}
                </div>
              )}
            </>
          ) : (
            <div className="muted pad">legacy / не представимо текущей семантической моделью</div>
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
            automations={s.semAutomations}
            onClose={() => setEditing(null)}
          />
        </Modal>
      )}
    </section>
  )
}
