# ESP32-C3 Wi-Fi CP (ESP-Hosted, SPI)

Прошивка внешнего **ESP32-C3** — Wi-Fi-сопроцессора для ESP32-P4. P4 общается с
C3 по **ESP-Hosted** (SPI full-duplex). Это отдельный ESP-IDF-проект; в Cargo
workspace V3 он не входит.

- ESP-Hosted: **3.0.10** (host V3 — тоже 3.0.10, RPC V2)
- target: `esp32c3`, 4 MB flash
- SPI: full-duplex, mode 3, 40 MHz
- основа: `esp_hosted/examples/wifi/sta/cp` (Apache-2.0)

## Пины

| Сигнал | P4 (JP1) | C3 |
|---|---:|---:|
| SPI CLK | 32 | 6 |
| SPI MOSI | 28 | 7 |
| SPI MISO | 33 | 5 |
| SPI CS | 31 | 10 |
| HANDSHAKE (CP→P4) | 49 | 4 |
| DATA_READY (CP→P4) | 50 | 3 |
| RESET (P4→C3 EN) | 34 | EN / CHIP_PU |
| GND | GND | GND |

Питание C3 — отдельное (USB/5V), с P4 только общий GND: линк чувствителен к
просадкам при переэнумерации P4. Пины P4 заданы в
`crates/ha-app/sdkconfig.defaults` (`CONFIG_ESP_HOSTED_HOST_SPI_*`,
`...HANDSHAKE_GPIO`, `...DATA_READY_GPIO`, `...RESET_GPIO`).

**Не используй generic-дефолты C3** (`MISO2/HS3/DR4`) — здесь они переопределены
в `sdkconfig.defaults`.

## Сборка

Нужен IDF **6.1** и тот же тулчейн, что для P4 (см. `docs/build/BUILD.md` §1).
`idf.py` должен быть из этого окружения.

```powershell
cd tools/esp_hosted_c3_cp
idf.py set-target esp32c3
idf.py build
```

Первый build скачает `espressif/esp_hosted` 3.0.10 (component manager).

Если менял пины/`sdkconfig.defaults` или `sdkconfig` уже создан под другой
target, пересоздай конфиг:

```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py fullclean
idf.py set-target esp32c3
idf.py build
```

## Прошивка

```powershell
idf.py -p COM12 flash
```

В текущей сборке C3 — **COM12** (проверено: `espflash board-info` → `esp32c3`).
P4 — **COM11**, не перепутать. Порт может отличаться на другой машине; проверяй
`espflash board-info` (должен быть `esp32c3`).

C3 шьётся по своему USB; P4 при этом не трогается. После прошивки C3
перезагрузи P4, чтобы хост заново поднял линк:

```powershell
espflash monitor --port COM11
```

Ожидаемые строки:

```
RPC version negotiated: V2
esp-hosted fw versions: host=3.0.10 coprocessor=3.0.10 (match)
ha_net: station started, connecting to '...'
ha_app: wifi ip=...
```

Если версии не совпали — обнови и хост, и CP до одной версии
(`main/idf_component.yml` здесь и managed-компонент в V3), затем пересобери
оба. Смешивать 3.0.9/3.0.10 нельзя.

## Файлы

- `sdkconfig.defaults` — роль CP, SPI-пины, RPC V2, 4 MB/OTA.
- `sdkconfig.defaults.esp32c3` — C3-специфика (CPU 160, Wi-Fi-буферы 10/32/32/16).
- `main/idf_component.yml` — версия ESP-Hosted (3.0.10).
- `partitions_eh_cp_ota_4m.csv` — таблица разделов 4 MB.

`sdkconfig`, `build/`, `managed_components/` и `dependencies.lock` не коммитятся;
источник истины — `sdkconfig.defaults` и `sdkconfig.defaults.esp32c3`.
