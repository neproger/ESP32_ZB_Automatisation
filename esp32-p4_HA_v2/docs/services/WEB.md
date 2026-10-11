# Web Service

Слой: **Services / Web (backend-for-frontend)**. Карта системы — `../ARCHITECTURE.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Web service — адаптер для браузера, симметричный Zigbee-сервису. Он **не** хранит
source of truth.

Он:

- получает события и понимает, какая entity изменилась;
- при необходимости читает entity через Domain;
- несёт canonical `key`/`record` **verbatim** (интерпретация — в браузере);
- пушит delta через WebSocket;
- при подключении клиента собирает snapshot;
- принимает команды/CRUD от браузера и переводит их в Domain-вызовы;
- решает фронтенд-особенности транспорта (формат кадра, доставка, версия).

Web — **тупой транспорт**: он не строит предметных DTO/проекций и не интерпретирует
Domain. Провод отдаёт canonical записи как есть, декодирует и осмысливает браузер (§5).

## 2. Граница модели

```text
canonical Domain records (ha_model) → verbatim binary WS → браузер (интерпретация)
```

`web` не собирает отдельный view: canonical форма уже и есть wire-форма
(little-endian, fixed-width), а вся семантика (имена, виджеты, единицы) — на клиенте.
Компактных DTO/кодеков в `web` нет.

## 3. Snapshot и delta

```text
client connected / SNAPSHOT
  → SYNC_BEGIN → UPSERT(type=ha_entity_t){key,record} × N → SYNC_END

ENTITY_UPSERTED → domain_get → UPSERT(key,record)     (broadcast)
ENTITY_REMOVED  → REMOVE(key)                          (broadcast)
EVENT           → ha_event_t verbatim                  (broadcast)
```

## 4. Транспорт: Wi-Fi

Wi-Fi web не принадлежит: у P4 нет своего радио, встроенный C6 занят 802.15.4, поэтому
радио поднимает **сервис `wifi`** (`WIFI.md`) на внешнем ESP32-C3 через `ESP-Hosted`
(UART). Web — только HTTP+WS; сервер стартует без IP и становится доступен, как только
`wifi` получит адрес. Пины/скорость — `../hardware/JC4880P443C_I_W.md` §4.5.

## 5. Протокол и клиент

Провод — `WEB_PROTOCOL.md` (canonical records verbatim: `type = ha_entity_t`,
`[key][record]`; маленький endian; 8‑байтный заголовок). Клиент — `web-ui/`
(Vite + Preact/JSX): кадры и encoders команд (`src/proto.js`), модель-стор
(`src/store.js`), семантика/словари (`src/semantics.js`, `src/automation.js`),
UI (`src/App.jsx`, `src/main.jsx`).

`npm run build` кладёт сборку в `web/ui/` (`index.html`, `app.js`, `app.css` и их `.gz`;
второй шаг — `scripts/gzip.mjs`), откуда прошивка встраивает её (`EMBED_FILES`) и отдаёт
по `/` — отдельный сервер не нужен, устройство отдаёт приложение само. Порядок:
`npm run build` (в `web-ui`) → `idf.py build`. Без собранного `web/ui/` прошивка отдаёт
заглушку. В дев — `npm run dev`, `VITE_WS_URL=ws://<ip>/ws` (или `?ws=`).

## 6. Wi-Fi provisioning

Креды Wi-Fi — данные Domain, а не UI. Три таблицы (`ha_model/ha_wifi.h`):

- `HA_ENTITY_WIFI_SCAN` — результат последнего скана (RAM); сервис чистит её после
  подключения и завершения работы;
- `HA_ENTITY_WIFI_KNOWN` — известные точки (ssid+password, FLASH): при старте сервис
  сканирует, среди известных выбирает сеть с самым сильным сигналом и подключается;
- `HA_ENTITY_WIFI_STATUS` — состояние подключения (RAM).

Экран Display читает эти таблицы и постит команды `HA_CMD_WIFI_SCAN` /
`HA_CMD_WIFI_CONNECT` (`ha_commands.h`); пароль, введённый на экране, сервис сохраняет в
известные. Сервис — отдельный компонент `wifi` (`WIFI.md`), владелец радио; web радио не
касается.

## 7. Открытые вопросы

- консистентность snapshot при параллельных writers (eventual consistency
  «дельты догонят» vs version-stamped snapshot).
