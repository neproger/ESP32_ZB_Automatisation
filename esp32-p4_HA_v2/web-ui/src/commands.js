// Словарь ZCL-команд: cluster → команды с реальными command id и параметрами.
// Источник — ZCL spec (на бэке те же числа — ha_zigbee.h). Виджет и кодирование
// аргументов берутся отсюда, а не из кода под каждую сущность. Бэкенд шлёт любой
// кадр (raw APS), поэтому список команд можно расширять только этим словарём.

const transition = { key: 'transition', label: 'переход, 0.1с', type: 'u16', default: 0 }
const level = { key: 'level', label: 'уровень', type: 'u8', min: 0, max: 254, default: 128 }

export const CLUSTERS = {
  0x0006: {
    name: 'On/Off',
    commands: [{ widget: 'onoff', name: 'On/Off', off: 0x00, on: 0x01, toggle: 0x02 }],
  },
  0x0008: {
    name: 'Уровень',
    commands: [{ widget: 'level', id: 0x00, name: 'Уровень', options: true, params: [level, transition] }],
  },
  0x0300: {
    name: 'Цвет',
    commands: [
      { widget: 'color_temp', id: 0x0a, name: 'Температура цвета', options: true, params: [transition] },
      { widget: 'color_xy', id: 0x07, name: 'Цвет XY', options: true, params: [transition] },
    ],
  },
  0x0003: {
    name: 'Identify',
    commands: [
      { widget: 'number', id: 0x00, name: 'Identify', params: [{ key: 'time', label: 'время, с', type: 'u16', min: 0, max: 0xffff, default: 10 }] },
    ],
  },
}

// ZCL payload: value u8/u16 + (для set-команд) OptionsMask+OptionsOverride (два нуля).
export function u8(v) {
  return [Math.max(0, Math.min(0xff, Math.round(v))) & 0xff]
}
export function u16(v) {
  const n = Math.max(0, Math.min(0xffff, Math.round(v))) & 0xffff
  return [n & 0xff, n >> 8]
}
export const OPTIONS = [0, 0]
