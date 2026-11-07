# Wi-Fi на ESP32-P4 + C3 CP: AUTH_EXPIRE и TX power

## Симптом

P4 (IDF 6.1) с официальным C3 CP (`esp_hosted` 3.0.10) видит AP, но
подключение циклически падает:

```
ha_net: disconnected: reason=2 (AUTH_EXPIRE (AP did not answer auth)) rssi=-56
```

При этом:

- C3 на IDF **5.5.1** подключается к тому же AP;
- обычный ESP32 (Xtensa) подключается;
- MAC C3 не заблокирован роутером (`access: permit`, MAC-фильтр `none`);
- Zigbee на P4 работает.

## Причина и проверенное решение

На стенде P4 + C3 CP IDF 6.1 помогает **ограничение TX power**. В
`native_net/ha_wifi.c` после `esp_wifi_start()` и до первого connect:

```c
#define WIFI_MAX_TX_POWER 34 /* 0.25 dBm units: 34 = 8.5 dBm */

const int8_t max_tx_power = WIFI_MAX_TX_POWER;
const esp_err_t tx_err = esp_wifi_set_max_tx_power(max_tx_power);
ESP_LOGI(TAG, "max tx power=%d (%.1f dBm): %s", max_tx_power,
         (double)max_tx_power * 0.25, esp_err_to_name(tx_err));
```

Проверенный лог:

```
ha_net: max tx power=34 (8.5 dBm): ESP_OK
...
ha_net: connecting to "<AP>" ...
esp_netif_handlers: sta ip: 192.168.1.x
ha_net: got ip: 192.168.1.x
ha_app: wifi ip=192.168.1.x
```

`AUTH_EXPIRE` исчезает, SNTP и погода стартуют. PMF, BSSID-pin, AMPDU и
протокол 802.11 при этом не менялись.

Проверка версий обязательна; host и CP должны совпадать:

```
eh_init_evt: esp-hosted fw versions: host=3.0.10 coprocessor=3.0.10 (match)
```

## IDF 6.1

В migration guide ESP-IDF 6.1 отдельного Wi-Fi-пункта нет — он касается только
`peripherals` и `tools`. Отдельного апстрим-фикса `AUTH_EXPIRE`/TX power в IDF
6.1 нет. Релевантные исправления Wi-Fi/транспорта пришли в `esp_hosted` 3.0.10
(он закреплён в проекте); сама мощность ограничивается на уровне платы.
Это не замена исправлению Espressif, а рабочий предел для этой разводки.

## Почему

Полная мощность (около 20 dBm) на C3/C6 в этой разводке/прошивке приводит к
сбою авторизации: AP не получает/не успевает обработать ответ, стек 6.1
сбрасывает auth по таймауту. Снижение мощности убирает самовоздействие/просадку
по питанию. Это не замена исправлению Espressif, а рабочий предел для платы.

## Подбор мощности

`esp_wifi_set_max_tx_power()` принимает шаги 0.25 dBm:

| Значение | dBm |
|---:|---:|
| 34 | 8.5 |
| 44 | 11 |
| 52 | 13 |
| 60 | 15 |
| 78 | 19.5 |
| 80 | 20 |

Рекомендация:

1. Оставить `34` и проверить сутки/ребуты/дальность.
2. Если не хватает дальности — поднимать по одному шагу: `44`, `52`, `60`.
   После каждого — перезагрузка, несколько циклов auth, лог на `AUTH_EXPIRE`.
3. Не прыгать сразу на 20 dBm: это возвращает исходный симптом.
4. Оставлять минимальное стабильное значение.

TX power влияет на запас линии/дальность и косвенно на retries/MCS. Скорость
PHY задают RSSI/SNR, ширина канала, AP и esp-hosted; при RSSI около −60 dBm
8.5 dBm обычно достаточно. Мощность AP (downlink) этим не меняется.

## Статус в UI и watchdog

`WIFI_STATUS` — единственный контракт для Display/Web.

Ключевой факт: на этой плате C3/esp_hosted 3.0.10 вызов
`esp_wifi_sta_get_ap_info()` уходит как `Req_WifiStaGetApInfo` (RPC `msg_id=294`)
и валит SPI-транспорт:

```
eh_host_feat_rpc: tx_worker: io_ops.tx_bytes failed (rc=-1 msg_id=294)
ha_net: scan_start failed: ESP_FAIL
```

После этого пропадают Wi-Fi, web и status, хотя P4 жив. Поэтому:

- `ha_wifi_status()` не вызывает `get_ap_info()`; он отдаёт event-driven
  `s_connected` (GOT_IP / DISCONNECTED) и кэш SSID/RSSI, заполненный при
  connect/scan;
- watchdog работает по `s_connected`, без периодического AP-info RPC; при
  `!s_connected` после grace-периода он запускает scan+connect;
- `link_up()` — это `s_connected`, а не `get_ap_info()`;
- Rust-сервис публикует status раз в секунду, Display/Web читают его из Domain.

RSSI в `WIFI_STATUS` — с последнего connect/scan (может быть слегка устаревшим);
свежий RSSI есть в `WIFI_SCAN`. Зато фоновый RPC 294 больше не убивает
транспорт. Если `get_ap_info()` когда-нибудь понадобится, сначала проверить,
что esp_hosted/C3 его чинит.

## Ограничения и долг

- Известные AP: persistent-запись `WIFI_KNOWN` живёт в Domain (`domain`
  partition, RamFlash), радио держит только RAM-копию (`s_known` в
  `native_net/ha_wifi.c`). Provisioning — с дисплея командой
  `COMMAND_WIFI_CONNECT`; `ha-wifi` сохраняет запись в Domain.
- Пароль закрыт от Web: `WIFI_KNOWN` не входит в `ha_web::SCHEMA`, а аргументы
  команд не пишутся в журнал (`COMMAND_SENT` без args).
- Zigbee (`native_zigbee/zigbee_poc.c`) использует отдельный раздел
  `zb_storage` (`POC_STORAGE_PARTITION`), не общий `nvs`.
- C3 CP и C6 display — отдельные прошивки; их NVS не относится к mstore/domain.
- C6 в этом стенде давал `0 AP` при сканировании (не auth); отдельно не
  проверялся и этим фиксом не покрыт. Если C6 Wi-Fi нужен — тестировать
  отдельно (5.5.1 / TX power / протокол).
