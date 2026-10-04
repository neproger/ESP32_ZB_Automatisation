import { store } from '../store.js'
import { zbCommand } from '../proto.js'
import { attrName, clusterName, formatValue, CLUSTER_ONOFF, CMD_ONOFF } from '../schema.js'

// Атрибут состояния: имя + значение; для On/Off — кнопки управления.
export default function StateAttr({ uid, st }) {
  const { key, record } = st
  return (
    <span className="attr">
      <b>{clusterName(key.cluster)} · {attrName(key.cluster, key.attr)}</b>
      <span className="val">{formatValue(key.cluster, key.attr, record.zclType, record.raw)}</span>
      {key.cluster === CLUSTER_ONOFF && (
        <>
          <button onClick={() => store.send(zbCommand({ uid, ep: key.ep, cluster: CLUSTER_ONOFF, command: CMD_ONOFF.OFF }))}>Выкл</button>
          <button onClick={() => store.send(zbCommand({ uid, ep: key.ep, cluster: CLUSTER_ONOFF, command: CMD_ONOFF.ON }))}>Вкл</button>
          <button onClick={() => store.send(zbCommand({ uid, ep: key.ep, cluster: CLUSTER_ONOFF, command: CMD_ONOFF.TOGGLE }))}>Toggle</button>
        </>
      )}
    </span>
  )
}
