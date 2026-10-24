# Аудит Z2M перед портом z2m-rs

Дата: 24.10.2026. Источники:

- `zigbee-herdsman-converters` **26.123.0** (локальный кэш V2, `.cache/zhc-26.123.0`);
- `zigbee-herdsman` **11.1.1** (там же, зависимость ZHC);
- `zigbee2mqtt` **2.14.2** (npm pack, только app-side `configure`/`receive`; ZHC 26.123 идёт
  с herdsman 11, т.е. соответствует более новой ветке Z2M — расхождение отмечено ниже).

Ссылки на файлы даны от корней пакетов, номера строк — из dist-сборок.

## 1. Приём кадра и Attribute Reports

**Что делает Z2M**

1. Адаптер (z-stack/ember/...) принимает ZCL-кадр и передаёт в контроллер; контроллер
   разбирает global-команды:
   `herdsman/dist/controller/controller.js:798-830`:
   - `report` → `type = "attributeReport"`, `data = ZclFrameConverter.attributeKeyValue(frame, ...)`;
   - `readRsp` → `type = "readResponse"`, тот же `attributeKeyValue`;
   - `read` → `attributeList`; `write` → `attributeKeyValue`;
   - для specific-команд: `type = "commandOn"`, `data = frame.payload`.
2. `attributeKeyValue` (`controller/helpers/zclFrameConverter.js:47-65`):
   - итерирует **весь** `frame.payload`, т.е. один кадр = **список атрибутов**;
   - записи `readRsp` с `status != SUCCESS` пропускаются (значения не синтезируются);
   - имя атрибута резолвится по кластеру и `manufacturerCode`; неизвестный атрибут
     остаётся под **числовым** id;
   - значение проходит `processAttributePostRead`; ошибка разбора атрибута логируется
     и атрибут пропускается;
   - есть Legrand workaround: если производитель Legrand, а кадр не помечен
     manufacturer-specific, кластер/атрибуты резолвятся по manufacturer ID устройства.
3. `endpoint.saveClusterAttributeKeyValue` сохраняет последние значения; затем эмитится
   **одно** событие `"message"` с объектом `data` (все атрибуты) и `meta`:
   `zclTransactionSequenceNumber`, `manufacturerCode`, `frameControl`, `rawData`
   (`controller.js:830-850`).
4. App-слой Z2M (`dist/extension/receive.js:137-194`):
   - конвертеры фильтруются по `cluster` и `type` (type может быть массивом);
   - кластеры `genOta/genTime/genBasic/genPollCtrl` без конвертера не считаются ошибкой;
   - каждый конвертер получает **весь** `data`; результаты мержатся в один payload;
   - исключение в конвертере логируется, остальные конвертеры продолжают работу.

**Следствие для нас.** Граница «один кадр — один атрибут» неверна. Транспорт должен
отдавать кадр как набор атрибутов (`attrId`, ZCL type, value bytes) из одного
Rust-буфера; `z2m-rs` получает по одному borrowed view. Повреждённый кадр
(не сходится длина, обрезан TLV) отвергается транспортом; корректный, но неизвестный
атрибут доходит до движка и может не дать обновлений (это не ошибка).

## 2. Транзакции, correlation, повторы

**Что делает Z2M**

- ZCL `transactionSequenceNumber` назначается herdsman helper'ом
  (`controller/helpers/zclTransactionSequenceNumber.js`), не конвертерами.
- Ожидание ответа — waitress с матчером
  (`adapter/adapter.js:109-123`): адрес (кроме Touchlink), cluster, endpoint,
  **tsn** (если задан), command id; defaultRsp матчится по `cmdId` из payload.
- Отправка (`adapter/z-stack/adapter/zStackAdapter.js:365-440`):
  - wait ставится до отправки, на `command.response` или `defaultRsp`;
  - data confirm; до **4** повторов для recoverable-ошибок
    (`NWK_NO_ROUTE`, `MAC_NO_ACK`, `MAC_CHANNEL_ACCESS_FAILURE`,
    `MAC_TRANSACTION_EXPIRED`, `BUFFER_FULL`, `MAC_NO_RESOURCES`);
  - для channel-access/buffer ошибок — пауза **2000 ms** перед повтором;
  - восстановление route/assoc, `DataConfirmTimeout` — синтетический код таймаута;
  - таймаут передаётся вызывающим (единого «магического» значения нет).
- Очередь запросов per-endpoint (`controller/helpers/requestQueue.js`): expiry,
  send policies `bulk/immediate/keep-payload/keep-command`, фильтрация/склейка
  ожидающих запросов; ошибка отправки не удаляет запрос до истечения срока.

**Следствие для нас.** Всё это — транспорт (`zigbee-esp`), не `z2m-rs`.
Минимально необходимый набор: waiters по `(nwk addr, endpoint, cluster, cmd, tsn)`,
таймаут, до 4 повторов с backoff 2 s для MAC/buffer-ошибок, per-endpoint очередь.
`z2m-rs` только строит ZCL/DP payload; tsn назначает транспорт.

## 3. Configure

**App-слой** (`z2m 2.14.2`, `dist/extension/configure.js:121-180`):

- `definition.configure(device, coordinatorEndpoint, definition)` вызывается как
  **код**, а не данные;
- single-flight на устройство (`configuring` set), счётчик попыток, максимум **3**
  (если не `force`);
- конфигурируются только `interviewed` и не `disabled`;
- **EndDevice только по `zigbee_event`** (т.е. когда проснулся/прислал сообщение);
  на старте — только routers, с паузой **10 s** между устройствами;
- успех фиксируется как `meta.configured = definition.version`; версия `0.0.0`
  не вызывает повторную конфигурацию;
- собственного таймаута у extension нет — работают таймауты herdsman-запросов;
- ошибка логируется; при `force/throw` пробрасывается.

**ZHC helpers** (`lib/reporting.js`, `lib/utils.js`):

- `bind(endpoint, target, clusters)` — последовательно по кластерам;
- `configureReporting` — payload `{attribute, min, max, change}`;
  константы `repInterval`: `HOUR=3600`, `MAX=65000`, `MINUTE=60`, `SECONDS_10=10`;
- многие helpers после reporting делают `read(...)` (например, battery);
- внутри helpers нет повторов/таймаутов — ошибка = reject;
- Tuya magic packet, произвольные чтения/записи и ветвления — **код**
  (`tuya.configureMagicPacket`), не данные.

**Следствие для нас.** `ConfigurePlan` как «чистый список шагов» покрывает
bind/read/write/report, но не все helpers. Нужен план операций, который может
содержать z2m-rs-функции, **не трогающие SDK**: `Op::Custom(fn(&mut PlanBuilder))`,
где функция только добавляет/меняет шаги. Исполнитель и планировщик — в
`zigbee-esp`/HA-слое. Для неперенесённых configure-функций профиль обязан иметь
`PARTIAL` и явный отчёт, как сейчас у TS011F.

## 4. Reporting

- Семантика: `min/max/change` в секундах/единицах ZCL; `MAX=65000`, `HOUR=3600`.
- Некоторые helpers: configureReporting + read; некоторые — только reporting.
- Golden-тесты должны проверять **последовательность** операций (bind/read/report,
  endpoint, attribute, интервалы), а не только итоговое значение.

## 5. Границы слоёв (итог аудита)

| Механизм | `z2m-rs` | `zigbee-esp` | HA Zigbee Layer |
|---|---|---|---|
| fingerprints, profiles, exposes | да | | |
| fromZigbee/toZigbee алгоритмы | да (Rust-порт) | | |
| ConfigurePlan (данные + `Custom`-шаги без SDK) | да | | |
| разбор ZCL/DP кадра, список атрибутов | | да | |
| tsn, waiters, таймауты, повторы, очереди | | да | |
| исполнение плана, sleepy-окна | | да | |
| single-flight/attempts/version/status конфигурации | | | да |
| нормализованные свойства → canonical HA | | | да |
| Domain/MicroDB/WebSocket/LVGL | | | выше |

`z2m-rs` не импортирует типы HA и ничего не знает про транспорт.

## 6. Что меняется в нашем коде

1. `RawZcl` заменяется на кадр с **набором** `AttributeView { id, zcl_type, value }`
   (borrowed из Rust-слота); `decode` вызывается на каждый view.
2. В `z2m-rs` добавляется `decode_attribute(profile, view)`; для команд остаётся
   текущий путь. Слияние обновлений — bounded-буфер, переполнение явное.
3. `zigbee-esp`: менеджер транзакций (waiters, повторы, per-endpoint очередь),
   конфигуратор с op-исполнителем и sleepy-планировщиком.
4. Генератор: для configure различать «последовательность стандартных ops» и
   «custom-код»; неперенесённое — `PARTIAL` + отчёт, без молчаливого упрощения.

## 7. Риски/открытые вопросы

- **Версии**: ZHC 26.123 (herdsman 11) vs app 2.14.2 (herdsman 10) — app-side
  configure мог измениться; зафиксировать версии в отчёте генератора.
- **Manufacturer-specific**: без полного ZCL-реестра имена неизвестных атрибутов
  не восстановить; храним числовой id (как Z2M при отсутствии имени).
- **Sleepy**: не блокировать слой; конфигурация возобновляется на следующем
  сообщении, максимум 3 попытки.
- **Tuya magic packet / partial helpers**: профиль не считается готовым, пока
  последовательность не воспроизведена; статус — `PARTIAL`.
- **Протокол HA**: преобразование в canonical-типы остаётся только в HA Zigbee
  Layer; `z2m-rs` от него не зависит.

## 8. Порядок работ (согласован)

1. Fixed buffer pool + parser атрибутов (host-тест: несколько атрибутов, битая длина,
   переполнение, возврат слота).
2. Интеграция с `zigbee-esp` и `z2m-rs` (одна копия на C-границе, дальше views).
3. Configure executor с fake transport (bind/read/report, таймаут/повтор,
   sleepy-отложенность).
4. Аппаратные Attribute Reports и configure.
5. Только затем — масштабирование каталога.
