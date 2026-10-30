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

## Ограничения и долг

- Известные AP сейчас хранятся в NVS (`ha_wifi`), provisioning — с дисплея.
  NVS — временный обход; по архитектуре их нужно перенести в Domain/mstore.
  Пароль в NVS/Domain должен быть закрыт от веб/журнала (redaction).
- Zigbee (`native_zigbee/zigbee_poc.c`) использует общий раздел `nvs`, а не
  выделенный `zb_storage`; `nvs_flash_erase()` может стереть и known AP.
  Нужен перевод Zigbee на `zb_storage` и отдельный `nvs_flash_init_partition`.
- C3 CP и C6 display — отдельные прошивки; их NVS не относится к mstore/domain.
- C6 в этом стенде давал `0 AP` при сканировании (не auth); отдельно не
  проверялся и этим фиксом не покрыт. Если C6 Wi-Fi нужен — тестировать
  отдельно (5.5.1 / TX power / протокол).
