// Семантический словарь (зеркало ha_model/ha_properties.h). НЕ ZCL: ни cluster, ни
// command id, ни scale. UI оперирует property/action/value; кодировку делает бэкенд.

export const PROPERTY = {
  UNKNOWN: 0,
  POWER: 1,
  BRIGHTNESS: 2,
  COLOR_HUE: 3,
  COLOR_SATURATION: 4,
  COLOR_X: 5,
  COLOR_Y: 6,
  COLOR_TEMPERATURE: 7,
  TEMPERATURE: 8,
  HUMIDITY: 9,
  ILLUMINANCE: 10,
  OCCUPANCY: 11,
  BATTERY_VOLTAGE: 12,
  BATTERY_PERCENT: 13,
  SYSTEM_TIME_UTC: 14,
  SYSTEM_TIME_STATUS: 15,
  SYSTEM_HOUR: 16,
  SYSTEM_MINUTE: 17,
  SYSTEM_WEEKDAY: 18,
  SYSTEM_WEEKDAY_MASK: 19,
  SYSTEM_MINUTES_OF_DAY: 20,
  SYSTEM_TZ_OFFSET: 21,
  COLOR: 22, // командная capability (состояние цвета — COLOR_X/COLOR_Y)
}

export const ACTION = { NONE: 0, ON: 1, OFF: 2, TOGGLE: 3, SET: 4 }

export const COMMAND_VALUE = { NONE: 0, SCALAR: 1, XY: 2 }
export const VALUE_KIND = { NONE: 0, BOOL: 1, I32: 2, U32: 3, FLOAT: 4, ENUM: 5 }

// Общий event vocabulary (не только Zigbee).
export const EVENT = {
  NONE: 0,
  SINGLE_PRESS: 1,
  DOUBLE_PRESS: 2,
  HOLD: 3,
  MINUTE_TICK: 4,
  HALF_HOUR_TICK: 5,
  HOUR_TICK: 6,
  DAY_TICK: 7,
  WEATHER_CHANGED: 8,
}
