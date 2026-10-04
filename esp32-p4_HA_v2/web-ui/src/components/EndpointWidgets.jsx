import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { zbCommand } from '../proto.js'
import { deriveEndpointMeta, hasReport } from '../capabilities.js'
import { formatAttrValue } from '../zcl.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'
import { CLUSTERS, u8, u16, OPTIONS } from '../commands.js'

function find(states, cluster, attr) {
  return states.find((s) => s.key.cluster === cluster && s.key.attr === attr) || null
}
function raw(states, cluster, attr) {
  const st = find(states, cluster, attr)
  return st ? st.record.raw >>> 0 : null
}

/* Контрол одной команды из словаря: виджет и кодирование args — по описанию команды. */
function CommandControl({ uid, ep, clusterId, cmd, states }) {
  const send = (command, args = []) => store.send(zbCommand({ uid, ep, cluster: clusterId, command, args }))

  if (cmd.widget === 'onoff') {
    const onoff = (raw(states, 0x0006, 0x0000) ?? 0) !== 0
    return (
      <div className="wrow">
        <label className="wlabel">
          <input type="checkbox" checked={onoff} onChange={(e) => send(e.target.checked ? cmd.on : cmd.off)} />
          Вкл
        </label>
        <button onClick={() => send(cmd.toggle)}>Toggle</button>
      </div>
    )
  }

  return <ParamControl uid={uid} ep={ep} clusterId={clusterId} cmd={cmd} states={states} send={send} />
}

/* Контролы с параметрами: level / color_temp / color_xy / number. */
function ParamControl({ cmd, states, send }) {
  if (cmd.widget === 'level') {
    const current = Math.max(0, Math.min(254, raw(states, 0x0008, 0x0000) ?? 0))
    return <LevelControl current={current} cmd={cmd} send={send} />
  }
  if (cmd.widget === 'color_temp') {
    const mired = raw(states, 0x0300, 0x0007) ?? 0
    const current = mired > 0 ? Math.max(2000, Math.min(6500, Math.round(1_000_000 / mired))) : 3000
    return <TempControl current={current} cmd={cmd} send={send} />
  }
  if (cmd.widget === 'color_xy') {
    const x = raw(states, 0x0300, 0x0003)
    const y = raw(states, 0x0300, 0x0004)
    const current = x != null && y != null ? xyToRgbHex(x, y) : '#ffffff'
    return <ColorControl current={current} cmd={cmd} send={send} />
  }
  if (cmd.widget === 'number') {
    return <NumberControl cmd={cmd} send={send} />
  }
  return null
}

function LevelControl({ current, cmd, send }) {
  const [value, setValue] = useState(current)
  useEffect(() => setValue(current), [current])
  const pct = Math.round((value / 254) * 100)
  return (
    <div className="wcol">
      <div className="wlabel">Уровень <b>{pct}%</b></div>
      <input type="range" min="0" max="254" value={value}
        onChange={(e) => { const v = Number(e.target.value); setValue(v); send(cmd.id, [...u8(v), ...u16(0), ...(cmd.options ? OPTIONS : [])]) }} />
    </div>
  )
}

function TempControl({ current, cmd, send }) {
  const [kelvin, setKelvin] = useState(current)
  useEffect(() => setKelvin(current), [current])
  return (
    <div className="wcol">
      <div className="muted">Темп. цвета: {kelvin} K</div>
      <input type="range" min="2000" max="6500" step="100" value={kelvin}
        onChange={(e) => { const k = Number(e.target.value); setKelvin(k); send(cmd.id, [...u16(1_000_000 / k), ...u16(0), ...(cmd.options ? OPTIONS : [])]) }} />
    </div>
  )
}

function ColorControl({ current, cmd, send }) {
  const [color, setColor] = useState(current)
  useEffect(() => setColor(current), [current])
  return (
    <div className="wcol">
      <div className="muted">Цвет</div>
      <input type="color" value={color}
        onChange={(e) => { setColor(e.target.value); const { x, y } = rgbHexToXy(e.target.value); send(cmd.id, [...u16(x), ...u16(y), ...u16(0), ...(cmd.options ? OPTIONS : [])]) }} />
    </div>
  )
}

function NumberControl({ cmd, send }) {
  const p = cmd.params[0]
  const [value, setValue] = useState(p.default ?? 0)
  const encode = p.type === 'u8' ? u8 : u16
  return (
    <div className="wrow">
      <span className="muted">{cmd.name}: {p.label}</span>
      <input className="ep" type="number" min={p.min} max={p.max} value={value} onChange={(e) => setValue(Number(e.target.value))} />
      <button onClick={() => send(cmd.id, encode(value))}>Отправить</button>
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
          <CommandControl key={`${clusterId}:${cmd.id ?? cmd.widget}`} uid={uid} ep={ep} clusterId={clusterId} cmd={cmd} states={states} />
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
