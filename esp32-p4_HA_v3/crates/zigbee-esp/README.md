# zigbee-esp (PoC)

Rust на ESP32-P4 по официальному шаблону `esp-rs/esp-idf-template` (ESP-IDF v6.1,
std, `riscv32imafc-esp-espidf`). На этом этапе — «Hello, world!» и проверка, что
связка toolchain + managed ESP-IDF + USB-консоль работает на нашей плате.

## Предпосылки (версии зафиксированы)

- rustup + `nightly-2026-07-08` с `rust-src` (пин в `rust-toolchain.toml`).
  Новее nightly ломает std для espidf на Windows: `libc::stat` без `st_atime`.
- `cargo install ldproxy espflash`.
- `libclang` для bindgen (Windows): `pip install libclang`, затем
  `LIBCLANG_PATH=<site-packages>\clang\native`.
- ESP-IDF ставится сам (`ESP_IDF_VERSION=v6.1`, `ESP_IDF_TOOLS_INSTALL_DIR=global`).

## Сборка и прошивка

```powershell
$env:LIBCLANG_PATH="$env:LOCALAPPDATA\Programs\Python\Python310\lib\site-packages\clang\native"
cargo build
espflash flash --port COM11 --baud 460800 target\riscv32imafc-esp-espidf\debug\zigbee-esp
```

`target-dir` вынесен в `C:/t/zbpoc` — esp-idf-sys не умеет длинные пути Windows
(репозиторий лежит глубоко). Это официальный обход из шаблона esp-rs.

## Особенности платы

- P4 rev v1.3 (pre-v3.0): `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` обязателен,
  иначе нестандартный CLIC даёт `Illegal instruction` в std.
- Консоль — USB-Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`).
- C6 прошит `ot_rcp` (802.15.4 RCP по UART), как в HA V2; менять не нужно.
