import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { semanticCommand } from '../proto.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'
import { CLUSTERS } from '../commands.js'
import { PROPERTY, ACTION } from '../semantics.js'

/* Текущее семантическое значение свойства (в человеческих единицах). */
function sem(uid, ep, property) {
  return store.semState(uid, ep, property)?.value
}

function CommandControl({ uid, ep, cmd }) {
  if (cmd.widget === 'onoff') {
    const onoff = sem(uid, ep, PROPERTY.POWER) === true
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
    const v = sem(uid, ep, PROPERTY.BRIGHTNESS)
    return <LevelControl uid={uid} ep={ep} current={v != null ? Math.round(v) : 0} />
  }
  if (cmd.widget === 'color_temp') {
    const v = sem(uid, ep, PROPERTY.COLOR_TEMPERATURE)
    return <TempControl uid={uid} ep={ep} current={v != null ? Math.round(v) : 3000} />
  }
  if (cmd.widget === 'color_xy') {
    const x = sem(uid, ep, PROPERTY.COLOR_X)
    const y = sem(uid, ep, PROPERTY.COLOR_Y)
    return <ColorControl uid={uid} ep={ep} current={x != null && y != null ? xyToRgbHex(x * 65535, y * 65535) : '#ffffff'} />
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

function sensor(uid, ep, property, fmt) {
  const v = sem(uid, ep, property)
  return v == null ? null : fmt(v)
}

export default function EndpointWidgets({ uid, ep, record }) {
  const serverClusters = (record.clusters || []).filter((c) => c.role === 1).map((c) => c.id)
  return (
    <div className="widgets">
      {serverClusters.map((clusterId) => {
        const def = CLUSTERS[clusterId]
        if (!def) return null
        return def.commands.map((cmd) => (
          <CommandControl key={`${clusterId}:${cmd.widget}`} uid={uid} ep={ep} cmd={cmd} />
        ))
      })}
      <div className="wrow">
        {sensor(uid, ep, PROPERTY.TEMPERATURE, (v) => <span className="sensor" key="t">Темп: <b>{v.toFixed(1)} °C</b></span>)}
        {sensor(uid, ep, PROPERTY.HUMIDITY, (v) => <span className="sensor" key="h">Влажн: <b>{v.toFixed(1)} %</b></span>)}
        {sensor(uid, ep, PROPERTY.BATTERY_PERCENT, (v) => <span className="sensor" key="b">Батарея: <b>{v.toFixed(0)} %</b></span>)}
        {sensor(uid, ep, PROPERTY.OCCUPANCY, (v) => <span className="sensor" key="o">Присутствие: <b>{v ? 'да' : 'нет'}</b></span>)}
      </div>
    </div>
  )
}
