import { useState } from 'react'
import { useStore } from '../useStore.js'
import { store } from '../store.js'
import { permitJoin } from '../proto.js'
import EntityCard from '../components/EntityCard.jsx'

const JOIN_SECONDS = 180

// Список логических сущностей (identity = entity_id, с именами). Без uid/endpoint.
export default function Entities() {
  const s = useStore()
  const [status, setStatus] = useState('')
  const list = [...s.semEntities.values()]
  list.sort((a, b) => (a.name || '').localeCompare(b.name || '') || (a.id < b.id ? -1 : 1))

  const openNetwork = () => {
    store.send(permitJoin(JOIN_SECONDS))
    setStatus(`Сеть открыта на ${JOIN_SECONDS} с — включите устройство в режиме подключения`)
  }

  return (
    <section>
      <h2>
        Сущности
        <button onClick={openNetwork}>Подключить устройства</button>
      </h2>
      {status && <p className="muted pad">{status}</p>}
      {list.length === 0 ? (
        <p className="muted pad">Сущностей пока нет</p>
      ) : (
        list.map((e) => <EntityCard key={e.id.toString()} ent={e} />)
      )}
    </section>
  )
}
