# Сборка HA V3

Три независимых контура:

- **host workspace** — `crates/` без `ha-app`;
- **ESP32-P4 приложение** — `crates/ha-app` с ESP-IDF и C-мостом;
- **C3 Wi-Fi CP** — `tools/esp_hosted_c3_cp` (ESP-Hosted-сопроцессор для Wi-Fi).

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
$env:HA_WIFI_SSID = '...'
$env:HA_WIFI_PASSWORD = '...'
.\tools\build-ha-app.ps1
```

Скрипт:

1. выставляет IDF v6.1, Python 6.1, `LIBCLANG_PATH`; Wi-Fi берёт из окружения
   (без переменных собирает прошивку без Wi-Fi);
2. собирает UI (`npm ci` при первом запуске, затем `npm run build`) — `web/ui`
   в gitignore, но встраивается в бинарник;
3. прогоняет CMake для `ha-app/native_zigbee` (C-изменения не отслеживаются Cargo);
4. запускает `cargo build`;
5. при падении удаляет только stale `esp-idf-sys` CMake cache и повторяет;
6. при повторном падении очищает `C:/t/hav3` и собирает заново.

Бинарь: `C:/t/hav3/riscv32imafc-esp-espidf/debug/ha-app`.

Обычная сборка — около 1–1.5 минуты; полная после очистки — около 3–4 минут.

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
