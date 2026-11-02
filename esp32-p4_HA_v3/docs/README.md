# Документация HA V2 → Rust

Главная задача проекта: **последовательно портировать проверенную реализацию
Home Assistant V2 на Rust**, сохраняя поведение и подключая слои к уже
существующему Domain.

## Нормативные документы

- [plan/V2_TO_RUST_PORTING_PRINCIPLES.md](plan/V2_TO_RUST_PORTING_PRINCIPLES.md)
  — главный принцип: V2 является эталоном, Rust — реализацией.
- [plan/V2_TO_RUST_PORTING_PLAN.md](plan/V2_TO_RUST_PORTING_PLAN.md)
  — единственный активный план и порядок этапов.
- [architecture/V3_LAYERS.md](architecture/V3_LAYERS.md) — контракт слоёв,
  владения и runtime.
- [architecture/TARGET_ARCHITECTURE.md](architecture/TARGET_ARCHITECTURE.md) —
  canonical-модель и потоки данных.

При расхождении по слоям/runtime действует `V3_LAYERS.md`; по canonical-
контракту — `TARGET_ARCHITECTURE.md`; по проверенному поведению — V2.

Исключение: Zigbee — особая реализация на основе V2 и Zigbee2MQTT (Z2M);
её архитектура может отличаться от V2. Полезные уже реализованные
Rust-улучшения сохраняются только после технической оценки. Подробности — в
плане портирования, разделы «Допустимые отличия от V2» и «Особенности
Zigbee-слоя».

## Аудиты и технические контракты

Это снимки и спецификации, они не заменяют активный план.

- [audits/V2_TO_V3_MAPPING.md](audits/V2_TO_V3_MAPPING.md) — историческая карта
  расхождений V2 → текущий Rust.
- [audits/HA_MODEL_AUDIT.md](audits/HA_MODEL_AUDIT.md) — canonical ABI и
  `ha-model`.
- [audits/MICRO_DB_AUDIT.md](audits/MICRO_DB_AUDIT.md) — Table Engine, Storage,
  Ring.
- [audits/FLASH_REGION_AUDIT.md](audits/FLASH_REGION_AUDIT.md) — V2 Flash,
  Region Manager, Ring.
- [audits/DOMAIN_PERSISTENCE_AUDIT.md](audits/DOMAIN_PERSISTENCE_AUDIT.md) —
  технический аудит первого слоя: Domain persistence/restore.
- [audits/DOMAIN_PARTITION_DIAGNOSTIC.md](audits/DOMAIN_PARTITION_DIAGNOSTIC.md) —
  read-only диагностика раздела `domain` до Stage 1.
- [audits/DOMAIN_RESET_PROCEDURE.md](audits/DOMAIN_RESET_PROCEDURE.md) —
  явный destructive reset `domain` после решения оператора.
- [audits/STAGE1_HARDWARE_VALIDATION.md](audits/STAGE1_HARDWARE_VALIDATION.md) —
  результаты аппаратной проверки Stage 1.
- [audits/DOMAIN_API_AUDIT.md](audits/DOMAIN_API_AUDIT.md) — соответствие
  Domain API V2 и текущие переходные элементы.
- [archive/DOMAIN_V2_MAPPING.md](archive/DOMAIN_V2_MAPPING.md) — исторический
  журнал переноса Domain ↔ z2m-rs.

## Исторические записи реализации

Не активные планы; полезны как обоснование уже принятых решений.

- [plan/STAGE_1C_ARCHITECTURE_DECISIONS.md](plan/STAGE_1C_ARCHITECTURE_DECISIONS.md)
- [plan/STAGE_1D_ARCHITECTURE_DECISIONS.md](plan/STAGE_1D_ARCHITECTURE_DECISIONS.md)
- [plan/STAGE_1D_IMPLEMENTATION_PLAN.md](plan/STAGE_1D_IMPLEMENTATION_PLAN.md)
- [plan/STAGE_2_ARCHITECTURE_DECISIONS.md](plan/STAGE_2_ARCHITECTURE_DECISIONS.md)
- [plan/STAGE_1D_4_P4_VALIDATION.md](plan/STAGE_1D_4_P4_VALIDATION.md) —
  протокол проверки на P4, ещё актуален.

## Сборка и платформа

- [build/BUILD.md](build/BUILD.md) — P4, host workspace, C3 Wi-Fi CP, ZHC compiler.
- [build/BUILD_ENV.md](build/BUILD_ENV.md) — окружение сборки P4.
- [build/WIFI.md](build/WIFI.md) — AUTH_EXPIRE и TX power на C3 CP.
- [hardware/JC4880P443C_I_W.md](hardware/JC4880P443C_I_W.md) — плата GUITION
  JC4880P443C_I_W: распиновка, дисплей, тач, радио.

## Zigbee / Z2M

- [zigbee/Z2M_AUDIT.md](zigbee/Z2M_AUDIT.md) — аудит Z2M перед портом `z2m-rs`.
- [zigbee/ZGM_SDK_TRANSPORT.md](zigbee/ZGM_SDK_TRANSPORT.md) — SDK transport и
  модель Request Executor.
- `zigbee/reports/` — инвентаризации ZHC, генератор, color/lighting/property.

## Внешний эталон

`esp32-p4_HA_v2/docs/` остаётся архитектурным и поведенческим эталоном V2.
Исходный код и тесты V2 не копируются в этот каталог и не заменяются
документацией Rust.

## Ревизия 2026-11-01

Удалены документы, которые описывали самостоятельную новую архитектуру или
отдельную систему миграции, а не портирование V2:

- `plan/STAGE_5_6_MIGRATION_PROTOCOL.md` — migration markers/recovery,
  отсутствующие в V2;
- `plan/STAGE_5_6_1_1_PROTOCOL_HARDENING.md` — дальнейшее развитие того же
  migration-протокола;
- `plan/STAGE_5_ARCHITECTURE_DECISIONS.md` — отдельная Stage 5 storage-политика
  и migration gate, расходящиеся с V2 backing;
- `plan/V3_IMPLEMENTATION_PLAN.md` — заменён единым
  `plan/V2_TO_RUST_PORTING_PLAN.md`.

Технические знания V2 сохранены в `audits/`, `architecture/` и V2-документации.
