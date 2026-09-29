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
