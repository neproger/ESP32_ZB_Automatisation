# Event Protocol v1

Документ фиксирует минимальный контракт обмена событиями между прошивкой и UI.

## 1. Envelope

Все события передаются в одном формате:

```json
{
  "ts_ms": 1730000000000,
  "type": "device.state",
  "data": {}
}
```

Поля:

- `ts_ms` (`u64`) - время события на устройстве (Unix ms).
- `type` (`string`) - тип события.
- `data` (`object`) - payload события.

## 2. Список типов событий

### 2.1 `automation.fired`

Когда правило прошло триггер и стартовало выполнение.

```json
{
  "automation_id": "auto_1"
}
```

`data`:

- `automation_id` (`string`)

### 2.2 `automation.result`

Когда правило завершило выполнение (успех/ошибка).

```json
{
  "ts_ms": 1730000000123,
  "type": "automation.result",
  "data": {
    "automation_id": "auto_1",
    "ok": true,
    "action_idx": 0,
    "err": ""
  }
}
```

`data`:

- `automation_id` (`string`)
- `ok` (`bool`)
- `action_idx` (`u32`, optional) - индекс action, на котором ошибка.
- `err` (`string`, optional) - короткий код/текст ошибки.

### 2.3 `device.event`

Командное/моментальное событие устройства (например, кнопка).

```json
{
  "ts_ms": 1730000000200,
  "type": "device.event",
  "data": {
    "device_id": "0x33fa65feffbd4d74",
    "endpoint_id": 1,
    "event": "button.press",
    "cmd": "toggle"
  }
}
```

`data`:

- `device_id` (`string`)
- `endpoint_id` (`u8`)
- `event` (`string`) - нормализованное имя события.
- `cmd` (`string`, optional)

### 2.4 `device.state`

Изменение состояния устройства.

```json
{
  "ts_ms": 1730000000300,
  "type": "device.state",
  "data": {
    "device_id": "0x0cc432556d38c1a4",
    "endpoint_id": 2,
    "key": "onoff",
    "value": true
  }
}
```

`data`:

- `device_id` (`string`)
- `endpoint_id` (`u8`)
- `key` (`string`) - ключ состояния (`onoff`, `temperature_c`, `humidity_pct`, ...).
- `value` (`bool | number | string | null`)

## 3. Типы бэка (C)

Рекомендуемый минимальный контракт в прошивке:

```c
typedef struct {
    uint64_t ts_ms;
    const char *type;   // "automation.fired", ...
    // payload формируется по type
} gw_evt_envelope_t;
```

Payload-модели:

```c
typedef struct {
    const char *automation_id;
} gw_evt_automation_fired_t;

typedef struct {
    const char *automation_id;
    bool ok;
    uint32_t action_idx;     // valid if has_action_idx
    bool has_action_idx;
    const char *err;         // optional
} gw_evt_automation_result_t;

typedef struct {
    const char *device_id;
    uint8_t endpoint_id;
    const char *event;
    const char *cmd;         // optional
} gw_evt_device_event_t;

typedef enum {
    GW_EVT_VALUE_NULL = 0,
    GW_EVT_VALUE_BOOL,
    GW_EVT_VALUE_NUM,
    GW_EVT_VALUE_TEXT,
} gw_evt_value_type_t;

typedef struct {
    gw_evt_value_type_t type;
    bool b;
    double n;
    const char *s;
} gw_evt_value_t;

typedef struct {
    const char *device_id;
    uint8_t endpoint_id;
    const char *key;
    gw_evt_value_t value;
} gw_evt_device_state_t;
```

## 4. Типы фронта (JavaScript, JSDoc)

```js
/**
 * @typedef {"automation.fired"|"automation.result"|"device.event"|"device.state"} EventType
 */

/**
 * @typedef {Object} EventEnvelope
 * @property {number} ts_ms
 * @property {EventType} type
 * @property {Object} data
 */

/**
 * @typedef {Object} AutomationFiredData
 * @property {string} automation_id
 */

/**
 * @typedef {Object} AutomationResultData
 * @property {string} automation_id
 * @property {boolean} ok
 * @property {number=} action_idx
 * @property {string=} err
 */

/**
 * @typedef {Object} DeviceEventData
 * @property {string} device_id
 * @property {number} endpoint_id
 * @property {string} event
 * @property {string=} cmd
 */

/**
 * @typedef {boolean|number|string|null} DeviceStateValue
 */

/**
 * @typedef {Object} DeviceStateData
 * @property {string} device_id
 * @property {number} endpoint_id
 * @property {string} key
 * @property {DeviceStateValue} value
 */
```

## 5. Bootstrap UI

При старте страницы:

1. Открыть WS.
2. Загрузить snapshot REST:
- `/api/devices`
- `/api/automations`
3. Применять события из WS к глобальному store.

Примечание:

- `/api/devices` должен включать не только метаданные устройств, но и актуальные состояния endpoint (`states`), чтобы не нужен был отдельный `/api/state`.

## 6. Про raw payload

`raw` в базовый протокол не входит.

Причины:

- Увеличивает CPU/RAM/трафик на C6.
- Не нужен для базовой логики UI и автомаций.

Допускается только отдельный debug-режим.
