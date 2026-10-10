import { clusterName, attrName, formatAttrValue } from '../zcl.js'

// Сырой Zigbee-атрибут (диагностика, read-only): только имя + значение. Управление —
// через canonical Entity UI (PROPERTY_COMMAND), не по raw-каналу.
export default function StateAttr({ st }) {
  const { key, record } = st
  return (
    <span className="attr">
      <b>{clusterName(key.cluster)} · {attrName(key.cluster, key.attr)}</b>
      <span className="val">{formatAttrValue(key.cluster, key.attr, record.zclType, record.raw)}</span>
    </span>
  )
}
