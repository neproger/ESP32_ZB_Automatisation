import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { automationPut, automationRemove } from '../proto.js'
import { describeTrigger, describeCondition, describeAction } from '../automation.js'
import AutomationForm from '../components/AutomationForm.jsx'
import Modal from '../components/Modal.jsx'
import Switch from '../components/Switch.jsx'

// Правила entity-native: описываются именами Entity, без uid/ep. Вкл/выкл — поле
// rule.enabled: сервер держит выключенные правила вне индекса, поэтому они не срабатывают.
export default function Automations() {
  const s = useStore()
  const [editing, setEditing] = useState(null) // null | 'new' | id-string
  const nameOf = (entity) => s.semEntities.get(entity.toString())?.name || entity.toString().slice(-6)
  const list = [...s.semAutomations.values()].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))
  const online = s.status === 'online'

  // Свич шлёт правило целиком с новым enabled (round-trip декодированной формы).
  const setEnabled = (a, enabled) => store.send(automationPut(a.id, { ...a.rule, enabled }))

  return (
    <section>
      <h2>
        Автоматизации <span className="muted">{list.length}</span>
        <button onClick={() => setEditing('new')}>Добавить</button>
      </h2>

      {list.length === 0 && <p className="muted pad">правил пока нет</p>}

      {list.map((a) => (
        <div className={`card rule${a.rule.enabled ? '' : ' off'}`} key={a.id.toString()}>
          <div className="card-head">
            <Switch
              checked={a.rule.enabled}
              disabled={!online}
              onChange={(v) => setEnabled(a, v)}
            />
            <span className="rule-id">Правило #{a.id.toString()}</span>
            <span className="spacer" />
            <button className="ghost" onClick={() => setEditing(a.id.toString())}>Изменить</button>
            <button className="ghost danger" onClick={() => store.send(automationRemove(a.id))}>Удалить</button>
          </div>

          <div className="flow">
            <span className="muted">когда</span>
            {a.rule.triggers.map((t, i) => (
              <span className="node trigger" key={i}>{describeTrigger(t, nameOf)}</span>
            ))}
            <span className="arrow">→</span>
            {a.rule.actions.map((x, i) => (
              <span className="node action" key={i}>{describeAction(x, nameOf)}</span>
            ))}
          </div>

          {a.rule.conditions?.length > 0 && (
            <div className="conds">
              <span className="muted">если</span>
              {a.rule.conditions.map((c, i) => (
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
            automations={s.semAutomations}
            onClose={() => setEditing(null)}
          />
        </Modal>
      )}
    </section>
  )
}
