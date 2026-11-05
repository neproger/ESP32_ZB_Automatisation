# Stage 3 — Hardware Validation

> Статус: **Host Verified / Hardware Blocked (acceptance; P4 boot resolved)**.
> Плата: ESP32-P4, COM11, MAC `80:f1:b2:d1:18:77`.
> Дата: 2026-11-02; boot resolved 2026-11-03.

## Что проверено

- Считана фактическая partition table с устройства: `factory` 8M,
  `mstore` 384K, `zb_storage` 16K, `domain` 512K.
- Build config исправлен на 16MB (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`);
  firmware пересобран.
- Bootloader, правильная 8M partition table и application image прошиты
  повторно. Data-разделы `mstore`/`domain` не стирались.
- Ошибка `partition ... exceeds flash chip size` больше не появляется.

## Boot blocker resolved

Причина зависания: был прошит промежуточный CMake-образ
`out/build/libespidf.bin` с пустым `app_main` (`ret`), а не финальный
Cargo/ESP-IDF образ `debug/ha-app`. После прошивки правильного образа
P4 стартует:

- `ha_app::store: registry ready`;
- `ha_app::web: web server on :80 (/ws)`;
- `zb_poc: stack started`;
- `ha_app::display: display started`;
- `ha_app ready`;
- Wi-Fi подключён, IP `192.168.1.100`.

C3 ESP-Hosted при этом исправен; повторная прошивка C3 не требуется.

## Что не проверено

- configure → binding confirm → command;
- restart → configure → command;
- deferred leave при ошибке persistence;
- reconciliation реальных ON/OFF/COLOR report;
- `0x43`;
- переполнение REMOVALS queue на устройстве.

## Следующее действие

Приложение на P4 запускается. Выполнить P4 checklist из
`STAGE_3_IMPLEMENTATION_SPEC.md` на реальных устройствах.