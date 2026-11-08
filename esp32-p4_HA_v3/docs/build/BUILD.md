# Сборка HA V3

Три независимых контура:

- **host workspace** — `crates/` без ESP-крейтов `ha-app`, `ha-web-service`, `ha-display`, `ha-zigbee-service`;
- **ESP32-P4 приложение** — `crates/ha-app` с ESP-IDF и C-мостом;
- **C3 Wi-Fi CP** — `tools/esp_hosted_c3_cp` (ESP-Hosted-сопроцессор для Wi-Fi).

Wi-Fi/AUTH_EXPIRE и подбор TX power: [WIFI.md](WIFI.md).

## 1. ESP32-P4

### Требования

- ESP-IDF **v6.1**: `C:\Users\ASUS\.espressif\esp-idf\v6.1`
- tools: `C:\Users\ASUS\.espressif`
- Python env: `C:\Users\ASUS\.espressif\python_env\idf6.1_py3.10_env`
- Rust target `riscv32imafc-esp-espidf`
- `espflash` в PATH (или `C:\Users\ASUS\.cargo\bin\espflash.exe`)

Пути уже закреплены в `crates/ha-app/.cargo/config.toml`:

```
IDF_PATH = .../esp-idf/v6.1            (force = true)
IDF_TOOLS_PATH = .../.espressif        (force = true)
IDF_PYTHON_ENV_PATH = .../idf6.1_py3.10_env (force = true)
target-dir = C:/t/hav3
```

`force = true` важен: глобальные `IDF_PATH`/`IDF_PYTHON_ENV_PATH` от v5.5
не должны перебивать v6.1. Не задавай эти переменные вручную.

### Сборка

Из корня `esp32-p4_HA_v3`:

```powershell
.\tools\build-ha-app.ps1
```

Скрипт:

1. выставляет IDF v6.1, Python 6.1, `LIBCLANG_PATH`; Wi-Fi при сборке не
   задаётся — сеть вводится на дисплее и хранится в NVS;
2. собирает UI (`npm ci` при первом запуске, затем `npm run build`) — `web/ui`
   в gitignore, но встраивается в бинарник;
3. прогоняет CMake для `ha-app/native_zigbee` (C-изменения не отслеживаются Cargo);
4. запускает `cargo build`;
5. при падении удаляет только stale `esp-idf-sys` CMake cache и повторяет;
6. при повторном падении очищает `C:/t/hav3` и собирает заново.

Бинарь: `C:/t/hav3/riscv32imafc-esp-espidf/debug/ha-app`.

Обычная сборка — около 1–1.5 минуты; полная после очистки — около 3–4 минут.

P4 lock instrumentation (opt-in):

```powershell
.\tools\build-ha-app.ps1 -Features p4-lock-metrics
```

Feature добавляет `p4-lock` probe и счётчики Domain lock. Протокол испытаний —
`docs/plan/STAGE_1D_4_P4_VALIDATION.md`.

### Прошивка

Кастомная таблица разделов обязательна:

```powershell
espflash flash --port COM11 --baud 921600 `
  --partition-table crates/ha-app/partitions.csv `
  C:/t/hav3/riscv32imafc-esp-espidf/debug/ha-app --monitor
```

Не включай `CONFIG_PARTITION_TABLE_CUSTOM*` в sdkconfig: таблица задаётся
флагом `espflash`.

Порты/платы:

- board A (V2, Wi-Fi C3): MAC `80:f1:b2:d3:62:7e`, обычно COM4 — не трогать;
- board B (P4 test): MAC `80:f1:b2:d1:18:77`, обычно COM11;
- C3 Wi-Fi CP: COM12 (`esp32c3`), не прошивать P4 через этот порт;
- C6 RCP: внутренний UART1 (GPIO14/15), не через COM12.

### Web/TCP (V2 parity)

- `CONFIG_LWIP_MAX_SOCKETS=12`, `httpd max_open_sockets=9` = 3 internal + 6
  web-клиентов; остаётся 3 сокета под DNS/weather.
- `CONFIG_LWIP_TCP_RECVMBOX_SIZE=16` обязателен. При `6` большие ответы
  (`app.js` 24.9 КБ) отдают заголовки, но тело встаёт; httpd-задача залипает.
- static/model отдаются чанками по 4 КБ: `MEMP_NUM_TCP_SEG=16`, один
  `httpd_resp_send` на 24 КБ блокирует TCP-очередь.
- HTTP-сервер стартует после `IP_EVENT_STA_GOT_IP`. Слушатель, созданный до
  выдачи IP, соединения не принимает.
- `httpd` stack — 16 КБ в internal RAM. При 32 КБ `httpd_start` возвращает
  `ESP_ERR_HTTPD_TASK` (`45064`).

### Типовые ошибки

**`extension 'xesppie' ... unsupported` / линковка `libespidf.elf`**

Причина: CMake cache от другого IDF/тулчейна. Лечение:

```powershell
.\tools\build-ha-app.ps1
```

Он сам удалит stale cache. Если чинишь вручную, удаляй только
`C:/t/hav3/riscv32imafc-esp-espidf/debug/build`, не исходники.

**В логах `Using esp-idf v5.5.1`**

Значит, окружение перебило `.cargo/config`. Проверь, что запускаешь из
`crates/ha-app`/через скрипт, и не выставляй `IDF_PATH` в терминале.

**Clean build: `ESP_HOSTED_HOST_USB_*_MANIFEST`**

`native_net/Kconfig` объявляет отключённые USB-транспорты ESP-Hosted, а
`sdkconfig.defaults` явно ставит их в `n`. Скрипт также выставляет
`IDF_COMPONENT_CHECK_NEW_VERSION=0`: component-manager 3.1.2 не разбирает
bool-правила `== 1` в новых registry-манифестах. Это отключает только
необязательную проверку новых версий, не Kconfig/сборочные проверки.
Если ошибка вернётся — проверь эти три места, не отключай проверки Kconfig.

**C3: `ESP_ERR_WIFI_STATE` / `12289`**

Не сбрасывай и не открывай C3 на COM12 (`espflash monitor`/`board-info`),
пока P4 работает: C3 теряет Wi-Fi init, и P4 начинает получать `12289` на
`scan`. C3 читается только raw serial на `921600`, без DTR/RTS. После сбоя —
полный power-cycle P4+C3 (выдернуть питание на 5 секунд), не reset-кнопка.

**Не пересобирается C-мост после правки `.c`**

Cargo не видит C-изменения. Скрипт явно вызывает CMake; вручную:

```powershell
$env:IDF_PATH='C:\Users\ASUS\.espressif\esp-idf\v6.1'
$env:IDF_TOOLS_PATH='C:\Users\ASUS\.espressif'
cmake --build C:/t/hav3/riscv32imafc-esp-espidf/debug/build/esp-idf-sys-*/out/build
```

## 1a. C3 Wi-Fi CP (ESP-Hosted)

C3 — внешний Wi-Fi-сопроцессор P4; отдельный ESP-IDF-проект, не Cargo:

```powershell
cd tools/esp_hosted_c3_cp
idf.py set-target esp32c3
idf.py build
idf.py -p COM12 flash
```

IDF — 6.1, как в §1. Версия `espressif/esp_hosted` (**3.0.10**) должна совпадать
с хостом V3. После прошивки C3 перезагрузи P4 и проверь в логе
`host=3.0.10 coprocessor=3.0.10 (match)`. Пины, питание и проверка — в
`tools/esp_hosted_c3_cp/README.md`.

## 2. Host workspace

Из `esp32-p4_HA_v3`:

```powershell
cargo test --workspace
```

`ha-app` исключён из workspace: у него отдельный ESP toolchain. Не добавляй его
в workspace members.

## 3. ZHC compiler (опционально)

Node **>=22.12**:

```powershell
cd tools/zhc-compiler
npm ci
npm run manifest
npm run coverage
npm run lighting
npm run sensors
npm run matcher
```

`node_modules/` и `out/` не коммитятся. ZHC закреплён в `package-lock.json`.
