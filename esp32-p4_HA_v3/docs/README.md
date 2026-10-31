# Документация HA V3

Каталог разложен по назначению. Нормативные документы — `architecture/` и `plan/`;
остальное — аудиты, сборка, исследования Zigbee и архив.

## Нормативные

- [architecture/V3_LAYERS.md](architecture/V3_LAYERS.md) — архитектурный контракт V3
  (главный документ при расхождении по слоям/runtime).
- [architecture/TARGET_ARCHITECTURE.md](architecture/TARGET_ARCHITECTURE.md) —
  canonical-модель, потоки данных, границы модулей.
- [plan/V3_IMPLEMENTATION_PLAN.md](plan/V3_IMPLEMENTATION_PLAN.md) — этапы 0–6,
  правила выполнения, контрактные тесты и DoD.

## Аудиты

Снимки состояния на дату; не заменяют нормативные документы.

- [audits/V2_TO_V3_MAPPING.md](audits/V2_TO_V3_MAPPING.md) — исходный аудит V2→V3.
- [audits/HA_MODEL_AUDIT.md](audits/HA_MODEL_AUDIT.md) — canonical ABI и `ha-model`.
- [audits/MICRO_DB_AUDIT.md](audits/MICRO_DB_AUDIT.md) — Table Engine, Storage, Ring.
- [audits/FLASH_REGION_AUDIT.md](audits/FLASH_REGION_AUDIT.md) — Flash, Region
  Manager, физическая раскладка.

## Сборка и платформа

- [build/BUILD.md](build/BUILD.md) — P4, host workspace, C3 Wi-Fi CP, ZHC compiler.
- [build/BUILD_ENV.md](build/BUILD_ENV.md) — окружение сборки P4.
- [build/WIFI.md](build/WIFI.md) — AUTH_EXPIRE и TX power на C3 CP.
- [hardware/JC4880P443C_I_W.md](hardware/JC4880P443C_I_W.md) — плата GUITION
  JC4880P443C_I_W: распиновка, дисплей, тач, радио (копия из V2).

## Zigbee / Z2M

- [zigbee/Z2M_AUDIT.md](zigbee/Z2M_AUDIT.md) — аудит Z2M перед портом `z2m-rs`.
- [zigbee/ZGM_SDK_TRANSPORT.md](zigbee/ZGM_SDK_TRANSPORT.md) — SDK transport и
  модель Request Executor.
- `zigbee/reports/` — инвентаризации ZHC, генератор, color/lighting/property.

## Архив

- [archive/DOMAIN_V2_MAPPING.md](archive/DOMAIN_V2_MAPPING.md) — исторический
  журнал переноса Domain ↔ z2m-rs. Не нормативный.

## Внешний эталон

`esp32-p4_HA_v2/docs/` остаётся архитектурным эталоном V2. Исключение —
`hardware/JC4880P443C_I_W.md`: справочник по плате скопирован из V2, потому что
на него ссылается `board_p4.h`.

## Ревизия 2026-10-31

Удалены как устаревшие/дублирующие (история в git):

- `ARCHITECTURE_BRIEF.md` — черновик 0.2; решения перенесены в `V3_LAYERS.md`
  и `TARGET_ARCHITECTURE.md`;
- `MICRODB_COMPACTION.md` — заменён `audits/FLASH_REGION_AUDIT.md` и
  `audits/MICRO_DB_AUDIT.md`.
