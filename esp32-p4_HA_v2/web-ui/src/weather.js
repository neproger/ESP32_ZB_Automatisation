// Погода системного сервиса: зеркало ha_model/ha_weather.h.
export const WEATHER_DEVICE_UID = 0x5754485200000001n

const CONDITION_NAMES = {
  0: '—',
  1: 'Ясно',
  2: 'Ясно',
  3: 'Переменная облачность',
  4: 'Облачно',
  5: 'Пасмурно',
  6: 'Туман',
  7: 'Дождь',
  8: 'Ливень',
  9: 'Снег',
  10: 'Снег с дождём',
  11: 'Град',
  12: 'Гроза',
  13: 'Гроза с дождём',
  14: 'Ветрено',
  15: 'Ветрено',
  16: 'Аномалия',
}

export function weatherConditionName(code) {
  return CONDITION_NAMES[code] || '—'
}

export function weatherKey(uid = WEATHER_DEVICE_UID) {
  return `weather:${uid}`
}
