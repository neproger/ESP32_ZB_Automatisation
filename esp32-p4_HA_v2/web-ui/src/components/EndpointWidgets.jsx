import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { semanticCommand } from '../proto.js'
import { deriveEndpointMeta, hasReport } from '../capabilities.js'
import { formatAttrValue } from '../zcl.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'
import { CLUSTERS } from '../commands.js'
import { PROPERTY, ACTION } from '../semantics.js'

function find(states, cluster, attr) {
  return states.find((s) => s.key.cluster === cluster && s.key.attr === attr) || null
}
function raw(states, cluster, attr) {
  const st = find(states, cluster, attr)
  return st ? st.record.raw >>> 0 : null
}

/* Контрол одного свойства: виджет — по описанию, отправка — семантическая команда. */
function CommandControl({ uid, ep, cmd, states }) {
  if (cmd.widget === 'onoff') {
    const onoff = (raw(states, 0x0006, 0x0000) ?? 0) !== 0
    const set = (action) => store.send(semanticCommand({ uid, ep, property: PROPERTY.POWER, action }))
    return (
      <div className="wrow">
        <label className="wlabel">
          <input type="checkbox" checked={onoff} onChange={(e) => set(e.target.checked ? ACTION.ON : ACTION.OFF)} />
          Вкл
        </label>
        <button onClick={() => set(ACTION.TOGGLE)}>Toggle</button>
      </div>
    )
  }
  if (cmd.widget === 'level') {
    const level = Math.max(0, Math.min(254, raw(states, 0x0008, 0x0000) ?? 0))
    return <LevelControl uid={uid} ep={ep} current={Math.round((level / 254) * 100)} />
  }
  if (cmd.widget === 'color_temp') {
    const mired = raw(states, 0x0300, 0x0007) ?? 0
    const current = mired > 0 ? Math.max(2000, Math.min(6500, Math.round(1_000_000 / mired))) : 3000
    return <TempControl uid={uid} ep={ep} current={current} />
  }
  if (cmd.widget === 'color_xy') {
    const x = raw(states, 0x0300, 0x0003)
    const y = raw(states, 0x0300, 0x0004)
    return <ColorControl uid={uid} ep={ep} current={x != null && y != null ? xyToRgbHex(x, y) : '#ffffff'} />
  }
  return null
}

function LevelControl({ uid, ep, current }) {
  const [value, setValue] = useState(current)
  useEffect(() => setValue(current), [current])
  const send = () => store.send(semanticCommand({ uid, ep, property: PROPERTY.BRIGHTNESS, action: ACTION.SET, value: Number(value) }))
  return (
    <div className="wcol">
      <div className="wlabel">Уровень <b>{value}%</b></div>
      <input type="range" min="0" max="100" value={value}
        onChange={(e) => setValue(Number(e.target.value))} onPointerUp={send} />
    </div>
  )
}

function TempControl({ uid, ep, current }) {
  const [kelvin, setKelvin] = useState(current)
  useEffect(() => setKelvin(current), [current])
  const send = () => store.send(semanticCommand({ uid, ep, property: PROPERTY.COLOR_TEMPERATURE, action: ACTION.SET, value: Number(kelvin) }))
  return (
    <div className="wcol">
      <div className="muted">Темп. цвета: {kelvin} K</div>
      <input type="range" min="2000" max="6500" step="100" value={kelvin}
        onChange={(e) => setKelvin(Number(e.target.value))} onPointerUp={send} />
    </div>
  )
}

function ColorControl({ uid, ep, current }) {
  const [color, setColor] = useState(current)
  useEffect(() => setColor(current), [current])
  const send = (hex) => {
    const { x, y } = rgbHexToXy(hex)
    store.send(semanticCommand({ uid, ep, property: PROPERTY.COLOR, action: ACTION.SET, value: { xy: { x: x / 65535, y: y / 65535 } } }))
  }
  return (
    <div className="wcol">
      <div className="muted">Цвет</div>
      <input type="color" value={color}
        onChange={(e) => { setColor(e.target.value); send(e.target.value) }} />
    </div>
  )
}

export default function EndpointWidgets({ uid, ep, record, states }) {
  const meta = deriveEndpointMeta(record)
  const serverClusters = (record.clusters || []).filter((c) => c.role === 1).map((c) => c.id)
  const sensor = (cluster, attr) => {
    const st = find(states, cluster, attr)
    return st ? formatAttrValue(cluster, attr, st.record.zclType, st.record.raw) : null
  }

  return (
    <div className="widgets">
      {serverClusters.map((clusterId) => {
        const def = CLUSTERS[clusterId]
        if (!def) return null
        return def.commands.map((cmd) => (
          <CommandControl key={`${clusterId}:${cmd.widget}`} uid={uid} ep={ep} cmd={cmd} states={states} />
        ))
      })}
      <div className="wrow">
        {hasReport(meta, 'temperature_c') && <span className="sensor">Темп: <b>{sensor(0x0402, 0x0000) ?? '—'}</b></span>}
        {hasReport(meta, 'humidity_pct') && <span className="sensor">Влажн: <b>{sensor(0x0405, 0x0000) ?? '—'}</b></span>}
        {hasReport(meta, 'battery_pct') && <span className="sensor">Батарея: <b>{sensor(0x0001, 0x0021) ?? '—'}</b></span>}
        {hasReport(meta, 'occupancy') && <span className="sensor">Присутствие: <b>{sensor(0x0406, 0x0000) ?? '—'}</b></span>}
      </div>
    </div>
  )
}
