import { useEffect, useMemo, useState } from 'react'
import { store } from '../store.js'
import { zbCommand } from '../proto.js'
import { deriveEndpointMeta, hasAccept, hasReport } from '../capabilities.js'
import { formatAttrValue } from '../zcl.js'
import { rgbHexToXy, xyToRgbHex } from '../color.js'

function find(states, cluster, attr) {
  return states.find((s) => s.key.cluster === cluster && s.key.attr === attr) || null
}
function raw(states, cluster, attr) {
  const st = find(states, cluster, attr)
  return st ? st.record.raw >>> 0 : null
}

function u16bytes(v) {
  const n = Math.max(0, Math.min(0xffff, Math.round(v))) & 0xffff
  return [n & 0xff, n >> 8]
}

export default function EndpointWidgets({ uid, ep, record, states }) {
  const meta = useMemo(() => deriveEndpointMeta(record), [record])

  const onoff = (() => {
    const r = raw(states, 0x0006, 0x0000)
    return r != null ? r !== 0 : false
  })()
  const level0 = Math.max(0, Math.min(254, raw(states, 0x0008, 0x0000) ?? 0))
  const colorX = raw(states, 0x0300, 0x0003)
  const colorY = raw(states, 0x0300, 0x0004)
  const color0 = colorX != null && colorY != null ? xyToRgbHex(colorX, colorY) : '#ffffff'
  const mired0 = raw(states, 0x0300, 0x0007) ?? 0
  const tempK0 = mired0 > 0 ? Math.max(2000, Math.min(6500, Math.round(1_000_000 / mired0))) : 3000

  const [level, setLevel] = useState(level0)
  const [color, setColor] = useState(color0)
  const [tempK, setTempK] = useState(tempK0)
  useEffect(() => setLevel(level0), [level0, uid, ep])
  useEffect(() => setColor(color0), [color0, uid, ep])
  useEffect(() => setTempK(tempK0), [tempK0, uid, ep])

  const send = (cluster, command, args = []) =>
    store.send(zbCommand({ uid, ep, cluster, command, args }))

  const sensor = (cluster, attr) => {
    const st = find(states, cluster, attr)
    return st ? formatAttrValue(cluster, attr, st.record.zclType, st.record.raw) : null
  }

  return (
    <div className="widgets">
      {hasAccept(meta, 'onoff.') && (
        <div className="wrow">
          <label className="wlabel">
            <input type="checkbox" checked={onoff} onChange={(e) => send(0x0006, e.target.checked ? 1 : 0)} />
            On
          </label>
          <button onClick={() => send(0x0006, 2)}>Toggle</button>
        </div>
      )}

      {hasAccept(meta, 'level.') && (
        <div className="wcol">
          <div className="muted">Уровень: {level}</div>
          <input
            type="range"
            min="0"
            max="254"
            value={level}
            onChange={(e) => {
              const v = Number(e.target.value)
              setLevel(v)
              send(0x0008, 0x00, [v, 0, 0])
            }}
          />
        </div>
      )}

      {hasAccept(meta, 'color.move_to_color_xy') && (
        <div className="wcol">
          <div className="muted">Цвет</div>
          <input
            type="color"
            value={color}
            onChange={(e) => {
              setColor(e.target.value)
              const { x, y } = rgbHexToXy(e.target.value)
              send(0x0300, 0x00, [...u16bytes(x), ...u16bytes(y), 0, 0])
            }}
          />
        </div>
      )}

      {hasAccept(meta, 'color.move_to_color_temperature') && (
        <div className="wcol">
          <div className="muted">Темп. цвета: {tempK} K</div>
          <input
            type="range"
            min="2000"
            max="6500"
            step="100"
            value={tempK}
            onChange={(e) => {
              const k = Number(e.target.value)
              setTempK(k)
              send(0x0300, 0x01, [...u16bytes(1_000_000 / k), 0, 0])
            }}
          />
        </div>
      )}

      <div className="wrow">
        {hasReport(meta, 'temperature_c') && <span className="sensor">Темп: <b>{sensor(0x0402, 0x0000) ?? '—'}</b></span>}
        {hasReport(meta, 'humidity_pct') && <span className="sensor">Влажн: <b>{sensor(0x0405, 0x0000) ?? '—'}</b></span>}
        {hasReport(meta, 'battery_pct') && <span className="sensor">Батарея: <b>{sensor(0x0001, 0x0021) ?? '—'}</b></span>}
        {hasReport(meta, 'occupancy') && <span className="sensor">Присутствие: <b>{sensor(0x0406, 0x0000) ?? '—'}</b></span>}
      </div>
    </div>
  )
}
