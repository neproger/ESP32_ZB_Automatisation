# Web Service

Слой: **Services / Web (backend-for-frontend)**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Web service — адаптер для браузера, симметричный Zigbee-сервису. Он **не** хранит
source of truth.

Он:

- получает события и понимает, какая entity изменилась;
- при необходимости читает entity через Domain;
- строит из canonical records удобный браузеру DTO (projection);
- пушит delta через WebSocket;
- при подключении клиента собирает snapshot;
- принимает команды/CRUD от браузера и переводит их в Domain-вызовы;
- решает фронтенд-особенности: batching, throttling, формат дат, capability projection,
  совместимость версий.

## 2. Разделение моделей

```text
Domain model → Web projection (DTO) → binary WS
```

а не `Domain struct == WebSocket packet`. Бинарность и эффективность сохраняем, но
проекция — отдельный слой. Все web-специфичные костыли остаются здесь и не лезут в Domain.

Пример: Domain хранит `device / endpoint / cluster / state`, а браузеру нужен собранный
view (`id`, `name`, `capabilities`, `state`). Его собирает Web service, Domain про такой
view не знает.

## 3. Snapshot и delta

```text
client connected
  → domain_list(...) → serialize DTO → SNAPSHOT_BEGIN/records/SNAPSHOT_END

ENTITY_UPSERTED → domain_get → Web DTO → STATE_DELTA → Browser
```

## 4. Открытые вопросы

- консистентность snapshot при параллельных writers (eventual consistency
  «дельты догонят» vs version-stamped snapshot).
