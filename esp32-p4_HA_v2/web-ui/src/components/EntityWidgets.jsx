import { useEffect, useState } from 'react'
import { store } from '../store.js'
import { propertyCommand } from '../proto.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'
import { PROPERTY, ACTION, propertyName, propertyWidget, propertyUnit } from '../semantics.js'

// Контролы Entity по её capability: свойство + действия. Identity — entity_id, без uid/ep.

function sem(entity, property) {
  return store.semState(entity, property)?.value
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

function ColorControl({ entity, current }) {
  const [color, setColor] = useState(current)
  useEffect(() => setColor(current), [current])
  const send = (hex) => {
    const { x, y } = rgbHexToXy(hex)
    store.send(propertyCommand({ entity, property: PROPERTY.COLOR, action: ACTION.SET, value: { xy: { x: x / 65535, y: y / 65535 } } }))
  }
  return (
    <div className="wcol">
      <div className="muted">Цвет</div>
      <input type="color" value={color} onChange={(e) => { setColor(e.target.value); send(e.target.value) }} />
    </div>
  )
}

function Capability({ entity, cap }) {
  const widget = propertyWidget(cap.property)
  const has = (a) => cap.actions.includes(a)
  const set = (property, action, value) => store.send(propertyCommand({ entity, property, action, value }))

  if (widget === 'switch') {
    const onoff = sem(entity, PROPERTY.POWER) === true
    return (
      <div className="wrow">
        <label className="wlabel">
          <input type="checkbox" checked={onoff}
            onChange={(e) => (e.target.checked ? has(ACTION.ON) && set(PROPERTY.POWER, ACTION.ON) : has(ACTION.OFF) && set(PROPERTY.POWER, ACTION.OFF))} />
          Вкл
        </label>
        {has(ACTION.TOGGLE) && <button onClick={() => set(PROPERTY.POWER, ACTION.TOGGLE)}>Toggle</button>}
      </div>
    )
  }
  if (widget === 'level') {
    const v = sem(entity, PROPERTY.BRIGHTNESS)
    return <SliderRow label="Уровень" unit="%" min={0} max={100} current={v != null ? Math.round(v) : 0}
      send={(val) => set(PROPERTY.BRIGHTNESS, ACTION.SET, val)} />
  }
  if (widget === 'color_temp') {
    const v = sem(entity, PROPERTY.COLOR_TEMPERATURE)
    return <SliderRow label="Темп. цвета" unit=" K" min={2000} max={6500} step={100} current={v != null ? Math.round(v) : 3000}
      send={(val) => set(PROPERTY.COLOR_TEMPERATURE, ACTION.SET, val)} />
  }
  if (widget === 'color') {
    const x = sem(entity, PROPERTY.COLOR_X)
    const y = sem(entity, PROPERTY.COLOR_Y)
    return <ColorControl entity={entity} current={x != null && y != null ? xyToRgbHex(x * 65535, y * 65535) : '#ffffff'} />
  }
  if (widget === 'indicator') {
    const v = sem(entity, cap.property)
    return <div className="wrow"><span className="sensor">{propertyName(cap.property)}: <b>{v == null ? '—' : v ? 'да' : 'нет'}</b></span></div>
  }
  const v = sem(entity, cap.property)
  const unit = propertyUnit(cap.property)
  return <div className="wrow"><span className="sensor">{propertyName(cap.property)}: <b>{v == null ? '—' : `${Number(v).toFixed(1)} ${unit}`}</b></span></div>
}

export default function EntityWidgets({ entity }) {
  const caps = store.capabilities(entity)
  if (caps.length === 0) return null
  return (
    <div className="widgets">
      {caps.map((c) => <Capability key={c.property} entity={entity} cap={c} />)}
    </div>
  )
}
