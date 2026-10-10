import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { semanticCommand } from '../proto.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'
import { PROPERTY, ACTION, propertyName, propertyWidget, propertyUnit } from '../semantics.js'

/* Текущее семантическое значение свойства (в человеческих единицах). */
function sem(uid, ep, property) {
  return store.semState(uid, ep, property)?.value
}

function SliderRow({ label, unit, min, max, step, current, send }) {
  const [value, setValue] = useState(current)
  useEffect(() => setValue(current), [current])
  return (
    <div className="wcol">
      <div className="wlabel">{label} <b>{value}{unit}</b></div>
      <input type="range" min={min} max={max} step={step} value={value}
        onChange={(e) => setValue(Number(e.target.value))}
        onPointerUp={() => send(Number(value))} />
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
      <input type="color" value={color} onChange={(e) => { setColor(e.target.value); send(e.target.value) }} />
    </div>
  )
}

/* Контрол по semantic capability: свойство + действия. Без cluster/commands.js. */
function Capability({ uid, ep, cap }) {
  const widget = propertyWidget(cap.property)
  const has = (a) => cap.actions.includes(a)

  if (widget === 'switch') {
    const onoff = sem(uid, ep, PROPERTY.POWER) === true
    const set = (action) => store.send(semanticCommand({ uid, ep, property: PROPERTY.POWER, action }))
    return (
      <div className="wrow">
        <label className="wlabel">
          <input type="checkbox" checked={onoff}
            onChange={(e) => (e.target.checked ? has(ACTION.ON) && set(ACTION.ON) : has(ACTION.OFF) && set(ACTION.OFF))} />
          Вкл
        </label>
        {has(ACTION.TOGGLE) && <button onClick={() => set(ACTION.TOGGLE)}>Toggle</button>}
      </div>
    )
  }
  if (widget === 'level') {
    const v = sem(uid, ep, PROPERTY.BRIGHTNESS)
    return <SliderRow label="Уровень" unit="%" min={0} max={100} current={v != null ? Math.round(v) : 0}
      send={(val) => store.send(semanticCommand({ uid, ep, property: PROPERTY.BRIGHTNESS, action: ACTION.SET, value: val }))} />
  }
  if (widget === 'color_temp') {
    const v = sem(uid, ep, PROPERTY.COLOR_TEMPERATURE)
    return <SliderRow label="Темп. цвета" unit=" K" min={2000} max={6500} step={100} current={v != null ? Math.round(v) : 3000}
      send={(val) => store.send(semanticCommand({ uid, ep, property: PROPERTY.COLOR_TEMPERATURE, action: ACTION.SET, value: val }))} />
  }
  if (widget === 'color') {
    const x = sem(uid, ep, PROPERTY.COLOR_X)
    const y = sem(uid, ep, PROPERTY.COLOR_Y)
    return <ColorControl uid={uid} ep={ep} current={x != null && y != null ? xyToRgbHex(x * 65535, y * 65535) : '#ffffff'} />
  }
  if (widget === 'indicator') {
    const v = sem(uid, ep, cap.property)
    return <div className="wrow"><span className="sensor">{propertyName(cap.property)}: <b>{v == null ? '—' : v ? 'да' : 'нет'}</b></span></div>
  }
  const v = sem(uid, ep, cap.property)
  const unit = propertyUnit(cap.property)
  return <div className="wrow"><span className="sensor">{propertyName(cap.property)}: <b>{v == null ? '—' : `${Number(v).toFixed(1)} ${unit}`}</b></span></div>
}

export default function EndpointWidgets({ uid, ep }) {
  const caps = store.endpointCapabilities(uid, ep)
  if (caps.length === 0) return null
  return (
    <div className="widgets">
      {caps.map((c) => <Capability key={c.property} uid={uid} ep={ep} cap={c} />)}
    </div>
  )
}
