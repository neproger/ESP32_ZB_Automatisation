# Архитектура C6 ↔ S3 ↔ UI

## 1. Текущая архитектура

### Роли узлов
- **ESP32-C6**: Zigbee-координатор и источник истины по метаданным устройств (NVS/storage).
- **ESP32-S3**: UI/HTTP/WS/автоматизации, кэш метаданных и обработка realtime-событий.
- **UI (web)**: отображение состояния, отправка команд, реакция на WS-события.

### Потоки данных

#### A. Метаданные устройств (топология, имена, endpoint-ы)
1. C6 хранит устройства в `device_storage` (NVS blob).
2. C6 формирует бинарный `DEVICE_FB` (device flatbuffer-подобный blob) и отправляет по UART чанками.
3. S3 принимает `DEVICE_FB`, собирает целый буфер и сохраняет в `device_fb_store`.
4. REST `GET /api/devices/flatbuffer` на S3 отдает этот blob в UI.
5. После успешного обновления blob S3 публикует событие `device.changed`.
6. UI по `gateway.event` + `event_type=device.changed` вызывает `loadDevices()`.

#### B. Realtime-события Zigbee
1. C6 получает Zigbee-события и публикует их в event bus.
2. C6 проксирует релевантные события по UART (`EVT`) на S3.
3. S3 преобразует их в локальные события (`zigbee.attr_report`, `zigbee.command` и т.д.).
4. `runtime_sync` на S3 обновляет `state_store`/`sensor_store`.
5. WS отправляет события в UI (`device.state`, `device.event`, `gateway.event`).

#### C. Команды управления
1. UI/REST на S3 отправляет команду в zigbee-uart слой.
2. S3 шлет `CMD_REQ` на C6.
3. C6 выполняет Zigbee-команду и возвращает `CMD_RSP`.

### Снимок/синхронизация
- Используется `SNAPSHOT` поток (begin/device/endpoint/end) для восстановления/проверки.
- После завершенного snapshot S3 запрашивает `SYNC_DEVICE_FB` для гарантии консистентного blob.

## 2. Что уже упрощено

- На C6 snapshot/blob больше собираются из registry/storage, а не из live `zb_model`.
- На S3 endpoint-ы для API идут через registry/storage-слой.
- Добавлена публикация `device.changed` после обновления `DEVICE_FB` на S3.
- Снижены риски stack overflow (крупные временные массивы переведены на heap в критичных местах).

## 3. Узкие места текущей схемы

1. **Дублирование каналов метаданных**:
   - одновременно используются `SNAPSHOT` и `DEVICE_FB`.
2. **Смешение “metadata changed” и “state changed”**:
   - часть логики обновлений может пересекаться.
3. **Много точек запуска отправки blob на C6**:
   - риск лишних отправок и сложной трассировки.
4. **Отсутствие ревизии blob**:
   - сложно быстро понять, новый ли blob пришел.

## 4. Рекомендуемые улучшения (приоритет)

### P1. Сделать `DEVICE_FB` главным каналом метаданных
- Оставить `SNAPSHOT` как fallback/debug.
- Основной update устройства/endpoint -> только через blob.

### P2. Ввести ревизию blob
- Добавить `revision`/`generation` в заголовок.
- S3/UI могут игнорировать повторные версии.

### P3. Единый debounce-планировщик отправки blob на C6
- Ввести `schedule_device_fb_push()` (например 200–500 мс).
- Все `join/leave/rename/simple_desc` только ставят флаг и триггерят планировщик.

### P4. Четко разделить события
- `device.changed` — только метаданные.
- `device.state.changed` (или текущий `device.state`) — только состояние.
- UI подписывается на нужный тип и минимально перезагружает данные.

### P5. Упростить recovery
- Если `DEVICE_FB` не собран/битый: один стандартный retry-путь (`SYNC_DEVICE_FB`) без каскада разных fallback.

## 5. Целевая простая модель

1. C6 изменил metadata в storage.
2. C6 отправил новый `DEVICE_FB` (debounced).
3. S3 применил blob и опубликовал `device.changed`.
4. UI по событию перезапросил `/api/devices/flatbuffer`.

Realtime состояния идут отдельно и не влияют на metadata-пайплайн.
