# S3 Automation Hub Migration (MVP)

This project now includes an initial backend stack copied from `ESP32-C6_Zigbee_Gateway`:

- `components/gw_core`
- `components/gw_http`
- `components/gw_zigbee` (stub transport adapter)
- `web/` (automation UI bundle/source)

## Current runtime model

`main/main.cpp` starts:

1. core stores and rules engine (`gw_core`)
2. Wi-Fi connect (`gw_wifi_connect_multi`)
3. HTTP/REST server (`gw_http_start`)
4. existing LVGL UI stack

## Important

`gw_zigbee` is currently a stub on S3 and returns `ESP_ERR_NOT_SUPPORTED` for Zigbee actions.
This keeps API/automation code compiling while transport to C6 is being implemented.

## Next implementation step

Replace `components/gw_zigbee/src/gw_zigbee_stub.c` with a UART transport client:

- encode command frames from S3 to C6
- send over UART with sequence + CRC
- parse ACK/RESULT and map to `ESP_OK`/error
- ingest state/event frames from C6 and publish into `gw_event_bus`

Once UART transport is implemented, S3 will become the automation/server brain and C6 will remain Zigbee radio gateway.
