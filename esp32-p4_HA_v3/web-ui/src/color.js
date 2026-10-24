// sRGB <-> CIE xy (порт v1) — общий код для виджетов и редактора автоматизаций.

const clamp01 = (v) => Math.max(0, Math.min(1, Number.isFinite(v) ? v : 0))
const srgbToLinear = (v) => {
  const x = v / 255
  return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4
}
const linearToSrgb = (v) => {
  const s = v <= 0.0031308 ? 12.92 * v : 1.055 * v ** (1 / 2.4) - 0.055
  return Math.max(0, Math.min(255, Math.round(s * 255)))
}

export function rgbHexToXy(hex) {
  const s = String(hex).replace('#', '')
  if (s.length !== 6) return { x: 0, y: 0 }
  const r = srgbToLinear(parseInt(s.slice(0, 2), 16))
  const g = srgbToLinear(parseInt(s.slice(2, 4), 16))
  const b = srgbToLinear(parseInt(s.slice(4, 6), 16))
  const X = r * 0.4124 + g * 0.3576 + b * 0.1805
  const Y = r * 0.2126 + g * 0.7152 + b * 0.0722
  const Z = r * 0.0193 + g * 0.1192 + b * 0.9505
  const sum = X + Y + Z
  return {
    x: Math.round(clamp01(sum > 0 ? X / sum : 0) * 65535),
    y: Math.round(clamp01(sum > 0 ? Y / sum : 0) * 65535),
  }
}

export function xyToRgbHex(xRaw, yRaw) {
  const x = xRaw / 65535
  const y = yRaw / 65535
  if (!(y > 0)) return '#ffffff'
  const X = x / y
  const Z = (1 - x - y) / y
  let r = X * 3.2406 - 1.5372 - Z * 0.4986
  let g = -X * 0.9689 + 1.8758 + Z * 0.0415
  let b = X * 0.0557 - 0.204 + Z * 1.057
  const m = Math.max(r, g, b, 1)
  r /= m
  g /= m
  b /= m
  const h = (n) => n.toString(16).padStart(2, '0')
  return `#${h(linearToSrgb(r))}${h(linearToSrgb(g))}${h(linearToSrgb(b))}`
}
