# Zigbee Service

Слой: **Services / Zigbee**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Zigbee service — адаптер к Zigbee-миру. Он знает Zigbee-модель и работает с ней, но не
хранит source of truth: состояние живёт в Domain.

Он:

- принимает reports/events от Zigbee и превращает их в изменения Domain
  (`upsert` state, `publish` event);
- исполняет команды, приходящие от Domain (напрямую, см. `../domain/COMMANDS.md`);
- не знает деталей других сервисов.

## 2. Поток

```text
Zigbee report
  → Zigbee service
  → Domain: upsert entity
  → Entity Store + Journal: ENTITY_UPSERTED
  → Dispatcher → subscribers
```

```text
Domain: post command
  → executor (Zigbee service)
  → отправка в Zigbee
  → Journal: COMMAND_SENT
```

## 3. Принцип

Zigbee semantics are authoritative (`../ARCHITECTURE.md`). Команда — намерение;
подтверждённое состояние приходит только из Zigbee; отсутствие репорта — отсутствие
факта, а не ошибка. Optimistic state и корреляция не вводятся.

## 4. Вне scope этого документа

Детали Zigbee-модели (endpoint / cluster / attribute), state machine интервью, удаление
устройства, адресация и взаимодействие с радио-шлюзом — отдельная тема.
