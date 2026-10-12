import { useEffect, useRef, useState } from 'react'
import { store } from '../store.js'
import { propertyCommand } from '../proto.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'
import { PROPERTY, ACTION, propertyName, propertyWidget, propertyUnit } from '../semantics.js'
import Switch from './Switch.jsx'

// Контролы Entity — переиспользуемые виджеты с вшитой логикой отправки. Identity — entity_id.
// Правило: команда уходит не на каждый шаг, а по завершению жеста —
//   * switch   — по факту переключения (как было);
//   * слайдер  — по отпусканию (onPointerUp/onKeyUp), не во время перетаскивания;
//   * цвет     — по подтверждению выбора (нативный 'change'), не потоком 'input'.
// Виджет сам держит локальное значение и коммитит один раз; домен не спамим.

function sem(entity, property) {
  return store.semState(entity, property)?.value
}

function set(entity, property, action, value) {
  store.send(propertyCommand({ entity, property, action, value }))
}

// --- переиспользуемые контролы ---

function PowerSwitch({ entity, cap }) {
  const has = (a) => cap.actions.includes(a)
  const onoff = sem(entity, PROPERTY.POWER) === true
  const change = (on) => {
    if (on && has(ACTION.ON)) set(entity, PROPERTY.POWER, ACTION.ON)
    else if (!on && has(ACTION.OFF)) set(entity, PROPERTY.POWER, ACTION.OFF)
    else if (has(ACTION.TOGGLE)) set(entity, PROPERTY.POWER, ACTION.TOGGLE)
  }
  return (
    <div className="wrow">
      <span className="wlabel">Вкл</span>
      <Switch checked={onoff} onChange={change} />
    </div>
  )
}

// Слайдер: держит значение локально, команду шлёт один раз по отпусканию.
function LevelSlider({ entity, property, label, unit, min, max, step = 1, fallback }) {
  const current = sem(entity, property)
  const [value, setValue] = useState(current != null ? Math.round(current) : fallback)
  useEffect(() => { if (current != null) setValue(Math.round(current)) }, [current])
  const commit = () => set(entity, property, ACTION.SET, Number(value))
  return (
    <div className="wcol">
      <div className="wlabel">{label} <b>{value}{unit}</b></div>
      <input type="range" min={min} max={max} step={step} value={value}
        onChange={(e) => setValue(Number(e.target.value))}
        onPointerUp={commit} onKeyUp={commit} />
    </div>
  )
}

// Цвет: нативные 'input' → превью; 'change' (закрытие/подтверждение выбора) → команда.
function ColorPicker({ entity }) {
  const x = sem(entity, PROPERTY.COLOR_X)
  const y = sem(entity, PROPERTY.COLOR_Y)
  const current = x != null && y != null ? xyToRgbHex(x * 65535, y * 65535) : '#ffffff'
  const [color, setColor] = useState(current)
  const ref = useRef(null)
  useEffect(() => setColor(current), [current])
  useEffect(() => {
    const el = ref.current
    if (el == null) return
    const commit = () => {
      const { x: cx, y: cy } = rgbHexToXy(el.value)
      store.send(propertyCommand({ entity, property: PROPERTY.COLOR, action: ACTION.SET, value: { xy: { x: cx / 65535, y: cy / 65535 } } }))
    }
    el.addEventListener('change', commit)
    return () => el.removeEventListener('change', commit)
  }, [entity])
  return (
    <div className="wcol">
      <div className="muted">Цвет</div>
      <input ref={ref} type="color" value={color} onChange={(e) => setColor(e.target.value)} />
    </div>
  )
}

function SensorCard({ label, value }) {
  return (
    <div className="sensor-card">
      <div className="sensor-card-label">{label}</div>
      <div className="sensor-card-value">{value}</div>
    </div>
  )
}

// --- проекция capability → контрол ---

function Capability({ entity, cap }) {
  const widget = propertyWidget(cap.property)
  if (widget === 'switch') return <PowerSwitch entity={entity} cap={cap} />
  if (widget === 'level') {
    return <LevelSlider entity={entity} property={PROPERTY.BRIGHTNESS} label="Уровень" unit="%" min={0} max={100} fallback={0} />
  }
  if (widget === 'color_temp') {
    return <LevelSlider entity={entity} property={PROPERTY.COLOR_TEMPERATURE} label="Темп. цвета" unit=" K" min={2000} max={6500} step={100} fallback={3000} />
  }
  if (widget === 'color') return <ColorPicker entity={entity} />

  /* Только чтение (нет действий) — sensor-карточка (как в HA v1). */
  const v = sem(entity, cap.property)
  const label = propertyName(cap.property)
  if (widget === 'indicator') return <SensorCard label={label} value={v == null ? '—' : v ? 'да' : 'нет'} />
  const text = v == null ? '—' : `${Number(v).toFixed(1)} ${propertyUnit(cap.property)}`.trim()
  return <SensorCard label={label} value={text} />
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
