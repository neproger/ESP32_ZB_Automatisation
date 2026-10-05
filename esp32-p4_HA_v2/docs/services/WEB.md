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
- решает фронтенд-особенности транспорта (формат кадра, совместимость версий). Проекция
  в DTO/capability не делается: провод отдаёт сырые записи, декодирует браузер (§5).

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

## 4. Транспорт: Wi-Fi

Wi-Fi web не принадлежит: у P4 нет своего радио, встроенный C6 занят 802.15.4, поэтому
радио поднимает **сервис `wifi`** (`WIFI.md`) на внешнем ESP32-C3 через `ESP-Hosted`
(UART). Web — только HTTP+WS; сервер стартует без IP и становится доступен, как только
`wifi` получит адрес. Пины/скорость — `../hardware/JC4880P443C_I_W.md` §4.5.

## 5. Протокол и клиент

Провод — `WEB_PROTOCOL.md` (вариант A: сырые записи Domain, без DTO/строк; маленький
endian; 8‑байтный заголовок). Клиент — `web-ui/` (Vite + Preact/JSX): схема записей
(`src/schema.js`), кадры (`src/proto.js`), словарь команд (`src/commands.js`), UI
(`src/App.jsx`, `src/main.jsx`).

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
  «дельты догонят» vs version-stamped snapshot);
- snapshot шлётся под Domain-lock (`WEB_PROTOCOL.md` §7);
- события (`EVENT`) и встраивание `dist/` в прошивку — следующие шаги.
