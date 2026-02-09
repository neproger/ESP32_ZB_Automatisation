# C6 UART Link (Thin Zigbee Router)

## Wiring (C6 <-> S3)
- `C6 GPIO5 (TX)` -> `S3 RX`
- `C6 GPIO4 (RX)` <- `S3 TX`
- `GND` <-> `GND`
- UART: `460800 8N1`

## Transport
- Text `JSON` per line (`\n` delimited, NDJSON)
- S3 -> C6: command frames
- C6 -> S3: event frames + ack

## S3 -> C6 commands

### `ping`
```json
{"type":"ping"}
```

### `zigbee.permit_join`
```json
{"type":"zigbee.permit_join","seconds":180}
```

### `zigbee.onoff`
```json
{"type":"zigbee.onoff","device_id":"0x33fa65feffbd4d74","endpoint_id":1,"cmd":"toggle"}
```

## C6 -> S3 ack
```json
{"type":"ack","ok":true,"req":"zigbee.onoff","err":""}
```

## C6 -> S3 events (forwarded from event bus)
```json
{
  "ts_ms": 1730000000000,
  "type": "zigbee.command",
  "data": {
    "device_id": "0x33fa65feffbd4d74",
    "short_addr": 16585,
    "endpoint_id": 1,
    "cmd": "toggle"
  }
}
```

Also forwarded:
- `zigbee.attr_report`
- `zigbee.cmd`
- `zigbee.cmd_sent`
- other `zigbee.*` events
