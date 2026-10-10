// Контролы устройства в семантике: cluster → controls с property (без ZCL command id/args).
// Кодирование команд делает бэкенд (semantics). Здесь — только форма виджета и свойство.
import { PROPERTY } from './semantics.js'

export const CLUSTERS = {
  0x0006: {
    name: 'On/Off',
    commands: [{ widget: 'onoff', name: 'On/Off', property: PROPERTY.POWER }],
  },
  0x0008: {
    name: 'Уровень',
    commands: [{ widget: 'level', name: 'Уровень', property: PROPERTY.BRIGHTNESS }],
  },
  0x0300: {
    name: 'Цвет',
    commands: [
      { widget: 'color_temp', name: 'Температура цвета', property: PROPERTY.COLOR_TEMPERATURE },
      { widget: 'color_xy', name: 'Цвет XY', property: PROPERTY.COLOR },
    ],
  },
}
