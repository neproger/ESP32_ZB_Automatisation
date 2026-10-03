# Automation Service

Слой: **Services / Automation**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Automation service — подписчик фактов Domain. При срабатывании правила он **постит
команду**, а не дёргает Zigbee напрямую.

- правила хранятся как обычные entity (`automation`) в Domain;
- при срабатывании публикуется `COMMAND` (см. `../domain/COMMANDS.md`);
- детали runtime правил — внутри сервиса.

## 2. Семантика (state-based)

Обычная автоматика работает по **текущему состоянию**:

```text
event (trigger/context)
  → Automation просыпается
  → читает актуальный state из Entity Store
  → оценивает правило
  → при срабатывании: command
```

То есть:

```text
Journal event = причина проснуться
Entity Store  = данные для решения
```

События при этом **не** схлопываются (`../domain/JOURNAL.md`): каждое доставляется и
считается отдельно. Если состояние успело измениться до пробуждения, правило просто
оценивается по актуальному состоянию.

## 3. Event-like события

События, которые нельзя выразить текущим state (`single_press`, `double_press`,
vendor event), публикуются как `EVENT` и обрабатываются через `value` / `payload_ref`
(`../domain/TRANSIENT_PAYLOAD.md`). Это не заставляет все state-события ходить через
payload ring.

## 4. Форма правила

Правило — entity `automation`; ключ — числовой id. Форма — в
`ha_model/include/ha_model/ha_automation.h`:

```text
trigger   событие (EVENT) от устройства: device_uid (0 — любое), command_id (0 — любая)
action    Zigbee-команда: device_uid (0 — устройство-источник), endpoint, cluster, command, args
```

Правило минимально: триггер по событию и действие-команда. `action_device_uid == 0`
означает «то же устройство, что вызвало событие».

## 5. Реализация

Компонент `automation` (только IDF) + `automation_rule.c` (чистая логика, host-тест
`test_rule`). Сервис подписан на факты `EVENT` от `ZIGBEE` (`kind_mask`, `source_mask`),
в задаче перебирает правила (`domain_entity_iter`) и при совпадении постит команду
(`domain_post(HA_CMD_ZIGBEE_CLUSTER, ...)`); мутирующий API Domain в try_push не
вызывается (`../domain/DOMAIN_API.md` §9).

**Временное правило bring-up.** `main/app_main.c` при старте засевает правило id=1
«нажатие кнопки → toggle реле (EP2) того же устройства» — чтобы сквозной путь был
проверяем до появления UI/Web. Идемпотентно (создаётся, если записи нет); убрать, когда
правила начнёт создавать UI.
