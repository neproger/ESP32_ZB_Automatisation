import { store } from '../store.js'
import { semanticCommand } from '../proto.js'
import { clusterName, attrName, formatAttrValue, CLUSTER_ONOFF } from '../zcl.js'
import { PROPERTY, ACTION } from '../semantics.js'

// Сырой атрибут состояния: имя + значение; для On/Off — семантические кнопки.
export default function StateAttr({ uid, st }) {
  const { key, record } = st
  const power = (action) =>
    store.send(semanticCommand({ uid, ep: key.ep, property: PROPERTY.POWER, action }))
  return (
    <span className="attr">
      <b>{clusterName(key.cluster)} · {attrName(key.cluster, key.attr)}</b>
      <span className="val">{formatAttrValue(key.cluster, key.attr, record.zclType, record.raw)}</span>
      {key.cluster === CLUSTER_ONOFF && (
        <>
          <button onClick={() => power(ACTION.OFF)}>Выкл</button>
          <button onClick={() => power(ACTION.ON)}>Вкл</button>
          <button onClick={() => power(ACTION.TOGGLE)}>Toggle</button>
        </>
      )}
    </span>
  )
}
