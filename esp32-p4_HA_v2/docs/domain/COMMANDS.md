# Commands

Слой: **Domain / Commands**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Команда — **transient intent**, а не состояние. Она не идёт через Journal как канал
доставки. У неё два независимых аспекта:

```text
delivery path  → напрямую в service/executor
audit path     → тонкая запись COMMAND_SENT в Journal
```

## 2. Доставка

```text
UI / Automation
  → domain_post(command, event_meta)
  → Domain маршрутизирует команду напрямую executor'у
  → Journal: COMMAND_SENT
  → Dispatcher
```

- Доставка executor'у — синхронная: `domain_post()` возвращается, когда команда передана.
  Результат выполнения не ожидается (fire-and-forget).
- Args команды не хранятся целиком: их получает исполнитель в момент dispatch.
  Маленький аргумент, важный для истории (`SET_LEVEL 32`), передаётся в `event_meta.value`
  и попадает в `COMMAND_SENT`.
- Если executor не найден или сразу отклонил команду — `domain_post()` возвращает
  обычную ошибку, а `COMMAND_SENT` не пишется. Отдельных diagnostic-фактов отказа нет.

## 3. Command Registry

Чтобы маршрутизация не была магией, есть маленький механизм регистрации:

```text
command_type → executor callback
```

Domain знает только discriminator и callback — ничего предметного. Это не слой,
а регистрация.

## 4. Семантика COMMAND_SENT

`COMMAND_SENT` означает **только одно**: Domain передал команду сервису для отправки.
В нём нет смысла «устройство получило / применило / подтвердило». Domain этого не ждёт.

Побочные эффекты строятся на **факте изменения состояния** entity, а не на завершении
команды:

```text
не «отправили ON и ждём», а «лампа сообщила ON → ENTITY_UPSERTED → подписчики».
```

## 5. Корреляция

Корреляцию `command → fact` не отслеживаем — это не HTTP RPC. UI не сопоставляет
отправку и последующий факт, а отображает текущее состояние. Pending (если нужен) —
локальная механика UI; Domain о нём не знает.

## 6. Граница: COMMAND vs request/response

```text
COMMAND = fire-and-forget intent (передали и результат не ждём)
```

Операции, которым вызывающему реально нужен ответ (`read_attr`, network scan,
`permit_join`, binding table) — **другой класс взаимодействия**. Он пока не решается
и не смешивается с COMMAND.
