// Системный девайс (время): зеркало ha_model/ha_system.h.
export const SYSTEM_DEVICE_UID = 0x53595354454d0001n
export const SYSTEM_ENDPOINT = 1
export const CLUSTER_SYSTEM = 0xfc00

export const SYS_ATTR = {
  HOUR: 0x0000,
  MINUTE: 0x0001,
  WEEKDAY: 0x0002,
  WEEKDAY_MASK: 0x0003,
  MINUTES_OF_DAY: 0x0004,
  TZ_OFFSET_MIN: 0x0005,
}

export const WEEKDAY_NAMES = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс']

// id состояния системного девайса в сторе (см. schema.js entityId).
export function systemStateId(attr) {
  return `st:${SYSTEM_DEVICE_UID}:${SYSTEM_ENDPOINT}:${CLUSTER_SYSTEM}:${attr}`
}
