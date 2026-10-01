# Статус

Где мы находимся, что проверено, что дальше. Карта системы — `ARCHITECTURE.md`;
контракты слоёв — в документах подсистем. Здесь только состояние работ.

## 1. Что сделано

| Компонент | Состояние | Проверено |
|---|---|---|
| `mstore` | Table + Ring, RAM/flash, полностью по `storage/MSTORE.md` | host-тесты 12/12 |
| `sys` | единая модель ошибки `sys_error_t` (`ERRORS.md`) | тестами mstore/domain |
| `domain` | Entity Store, Journal, Dispatcher, Transient Payload, Commands | host-тесты 9/9 |
| `ha_model` | словарь Zigbee/ZCL (`ha_zigbee.h`) + форма команды (`ha_commands.h`) | сборкой тестов |
| `domain/test_apps/domain_p4` | IDF-приложение: вертикальный срез на P4 | **не запускалось** — нет платы |
| `domain/CMakeLists.txt` | компонент ESP-IDF (`REQUIRES sys mstore esp_common esp_timer`) | **не собиралось** — нет IDF-сборки |

## 2. Что не проверено

```text
IDF-сборка компонента domain и приложения domain_p4
запуск вертикального сценария на железе (особенно FLASH-часть: нужен раздел mstore)
```

Host-проверка приложения ограничена компиляцией и линковкой с заглушкой `esp_log.h`:
сам `app_main` на хосте не запускался, потому что `flash_slice` требует реального
flash-устройства.

Как проверять (когда появится плата):

```bat
cd esp32-p4_HA_v2/domain/test_apps/domain_p4
idf.py set-target esp32p4
idf.py build flash monitor
```

Ожидаемый финал лога — `vertical slice OK`.

## 3. Что дальше

```text
1. P4: сборка и smoke всей цепочки            — ждёт плату
2. Стенд для наблюдения: консоль/демо Domain  — без новых архитектурных решений
3. Доменная часть ha_model: типы сущностей, layout ключа состояния, формы значений
4. Сервисы: Zigbee → Automation → Web
```

Порядок сервисов обусловлен тем, что Zigbee — единственный реальный источник
состояния; Web (BFF) и Automation потребляют то, что он produces. Пункт 3 нужен
любому сервису и не зависит от железа.

## 4. Открытые вопросы (где решение, а не здесь)

- **Адресат команды и события в факте** — `COMMAND_SENT` и `EVENT` пишутся без
  `entity`/`key`, поэтому подписчик не фильтрует их по устройству
  (`domain/COMMANDS.md` §8, `domain/TRANSIENT_PAYLOAD.md` §4).
- **Ключ состояния: числовой или семантический.** Определяет, живут ли
  Zigbee-идентификаторы в `ha_model` или уходят внутрь Zigbee-сервиса
  (`RECORD_MODEL.md` §10).
- **Типы сущностей и формы значений Domain** — определяются на реальной Zigbee
  state-модели, а не заранее (`RECORD_MODEL.md` §10).
- **`detail` в ошибке, границы layer-specific кодов** — `ERRORS.md` §5.

## 5. Как собирать и проверять локально

```text
mstore host-тесты   — см. команды в domain/tests/README.md, те же шаги для mstore/tests
domain host-тесты   — domain/tests/README.md
```

Оба набора обязательны перед коммитом: `-W4` на MSVC и `-Wall -Wextra -Werror` на
GCC/Clang, предупреждения недопустимы.
