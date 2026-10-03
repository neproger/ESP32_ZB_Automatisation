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

## 4. Транспорт: Wi-Fi через сопроцессор C3

У P4 нет своего Wi-Fi, а встроенный C6 занят 802.15.4 (Zigbee/RCP). Wi-Fi поднимается
на **отдельном ESP32-C3** через `ESP-Hosted` (транспорт UART, `web` — станция, креды в
`web/Kconfig.projbuild`). Пины, reset‑линия и скорость — `../hardware/JC4880P443C_I_W.md`
§4.5. `web_start` поднимает Wi‑Fi и HTTP+WS в своей задаче; при неудаче линка
веб‑сервер не стартует (bootstrap при этом продолжает работать на Zigbee).

## 5. Протокол и клиент

Провод — `WEB_PROTOCOL.md` (вариант A: сырые записи Domain, без DTO/строк; маленький
endian; 8‑байтный заголовок). Клиент — `web-ui/` (Vite, vanilla): схема записей
(`schema.js`), кадры (`proto.js`), WS + минимальный UI (`main.js`). Сборка — `npm run
build` (`dist/`); в дев — `npm run dev` и `VITE_WS_URL=ws://<ip>/ws`.

## 6. Открытые вопросы

- консистентность snapshot при параллельных writers (eventual consistency
  «дельты догонят» vs version-stamped snapshot);
- snapshot шлётся под Domain-lock (`WEB_PROTOCOL.md` §7);
- события (`EVENT`) и встраивание `dist/` в прошивку — следующие шаги.
