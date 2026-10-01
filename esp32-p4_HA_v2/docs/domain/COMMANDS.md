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

## 7. Контракт payload команды

Адресация живёт в payload, а не в Domain. Два уровня не смешиваются:

```text
Domain Command
    executor/type       — дискриминатор, по которому Domain выбирает executor
    payload             — opaque bytes для Domain

Zigbee command payload  — форма из ha_model (ha_commands.h)
    device_uid          — IEEE EUI-64, stable identity устройства
    dst_endpoint
    cluster_id          — из ha_zigbee.h
    command_id          — из ha_zigbee.h
    arguments           — уже в кодировке ZCL
```

Правила:

- `cluster_id` / `command_id` берём из словаря `ha_zigbee.h`, не изобретаем;
- `src_endpoint` — внутренняя политика Zigbee service/gateway, в контракте его нет;
- short address **не** является идентичностью команды: сервис сам разрешает
  `device_uid → current network address`;
- структуры `ezb_*` esp-zigbee-sdk создаются только внутри Zigbee service;
- Domain payload не разбирает: для него это байты. Масштабирование единиц
  (уровень, 0.01 °C) — тоже сторона сервиса.

## 8. Открытые вопросы

Решаются вместе с Zigbee-сервисом, а не заранее:

- ~~**Адресат команды в факте.**~~ Решено: фасад принимает адресата отдельным
  параметром — `domain_fact_target_t { entity, key }`. `COMMAND_SENT` несёт `entity` и
  ключ устройства, поэтому подписчик относит команду к устройству сам, сравнивая ключ
  доставленного события. Адресат проверяется **до** вызова executor'а: неизвестный тип
  — отказ `NOT_FOUND`, факта нет. Payload Domain по-прежнему не разбирает (§7).
- **Гранулярность дискриминатора.** Один `HA_CMD_*` на весь Zigbee-кластерный путь
  (`HA_CMD_ZIGBEE_CLUSTER`) или разбиение по классам команд — решит сервис по своему
  словарю (`RECORD_MODEL.md` §9).
- **Контекст вызова executor'а.** Executor вызывается синхронно в контексте
  вызывающего, поэтому для Zigbee он обязан копировать args и уходить в свою задачу;
  отправка в радио из чужого контекста недопустима. Проверить на реализации сервиса.
