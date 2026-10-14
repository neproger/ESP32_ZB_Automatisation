import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { automationRemove } from '../proto.js'
import { describeTrigger, describeCondition, describeAction } from '../automation.js'
import AutomationForm from '../components/AutomationForm.jsx'
import Modal from '../components/Modal.jsx'

// Правила entity-native: описываются именами Entity, без uid/ep.
export default function Automations() {
  const s = useStore()
  const [editing, setEditing] = useState(null) // null | 'new' | id-string
  const nameOf = (entity) => s.semEntities.get(entity.toString())?.name || entity.toString().slice(-6)
  const list = [...s.semAutomations.values()].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))

  return (
    <section>
      <h2>
        Автоматизации{' '}
        <button onClick={() => setEditing('new')}>Добавить</button>
      </h2>

      {list.length === 0 && <p className="muted pad">правил пока нет</p>}

      {list.map((a) => (
        <div className={`card${a.rule.enabled ? '' : ' off'}`} key={a.id.toString()}>
          <div className="card-head">
            <span className="rule-id">Правило #{a.id.toString()}</span>
            <span className="spacer" />
            <button className="ghost" onClick={() => setEditing(a.id.toString())}>Изменить</button>
            <button className="ghost danger" onClick={() => store.send(automationRemove(a.id))}>Удалить</button>
          </div>
          <div className="conds">
            <span className="muted">когда</span>
            {a.rule.triggers.map((t, i) => <span className="chip" key={i}>{describeTrigger(t, nameOf)}</span>)}
          </div>
          <div className="conds">
            <span className="muted">то</span>
            {a.rule.actions.map((x, i) => <span className="chip" key={i}>{describeAction(x, nameOf)}</span>)}
          </div>
          {a.rule.conditions?.length > 0 && (
            <div className="conds">
              <span className="muted">если</span>
              {a.rule.conditions.map((c, i) => <span className="chip" key={i}>{describeCondition(c, nameOf)}</span>)}
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
            automations={s.semAutomations}
            onClose={() => setEditing(null)}
          />
        </Modal>
      )}
    </section>
  )
}
