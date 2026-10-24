# Build environment (ESP32-P4)

## Symptom

A clean `esp-idf-sys` build failed in the CMake compiler test with:

```
riscv32-esp-elf-gcc.exe: error: '-march=rv32imafc_zicsr_zifencei_xesppie':
extension 'xesppie' starts with 'x' but is unsupported non-standard extension
```

## Cause

The shell exported IDF 5.5.1 (`IDF_PATH=C:\Users\ASUS\espidf\v5.5.1\esp-idf`).
A pre-existing `esp-idf-sys` build directory contained response files generated
under that IDF (`toolchain/ldflags` with `..._xesppie`). A later configure with
managed IDF 6.1 reused those flags. IDF 6.1 uses `..._xespv21` for P4 rev < 3.0,
so the flags and compiler did not match. GCC 15.2 is correct for IDF 6.1;
`xesppie` belongs to the IDF 5.5.1 toolchain (GCC 14.2).

## Fix

- Do not mix environments. For a managed IDF 6.1 build, unset
  `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH`, `ESP_IDF_TOOLS_INSTALL_DIR`.
- If a mixed build already happened, delete
  `<target>/riscv32imafc-esp-espidf/debug/build/esp-idf-sys-*` and rebuild.
- Keep `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` for the rev v1.3 board; it selects
  `..._xespv21`, not `xesppie`.
- No need to remove or replace GCC 15.2, and no need to delete IDF 5.x.
