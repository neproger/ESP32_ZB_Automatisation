# Stage 3 — Hardware Validation

> Статус: **Hardware Blocked / Host Verified**. Заблокировано на этапе загрузки приложения.
> Плата: ESP32-P4, COM11, MAC `80:f1:b2:d1:18:77`.
> Дата: 2026-11-02.

## Что проверено

- Считана фактическая partition table с устройства: `factory` 8M,
  `mstore` 384K, `zb_storage` 16K, `domain` 512K.
- Build config исправлен на 16MB (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`);
  firmware пересобран.
- Bootloader, правильная 8M partition table и application image прошиты
  повторно. Data-разделы `mstore`/`domain` не стирались.
- Ошибка `partition ... exceeds flash chip size` больше не появляется.

## Текущий блокер

После исправления flash config приложение доходит до:

```text
eh_auto_init: auto_init: initialising 'wifi' (priority 150)
```

и дальше не выдаёт `ha-app` логов; сеть `192.168.1.100` недоступна.
Это происходит до `main`/Stage 3 кода, поэтому P4-сценарии не выполнены.

## Что не проверено

- configure → binding confirm → command;
- restart → configure → command;
- deferred leave при ошибке persistence;
- reconciliation реальных ON/OFF/COLOR report;
- `0x43`;
- переполнение REMOVALS queue на устройстве.

## Следующее действие

Отдельной задачей исправить platform build config (16MB flash и custom
partition table), пересобрать firmware и повторить загрузку. После
успешного старта приложения выполнить P4 checklist из
`STAGE_3_IMPLEMENTATION_SPEC.md`.