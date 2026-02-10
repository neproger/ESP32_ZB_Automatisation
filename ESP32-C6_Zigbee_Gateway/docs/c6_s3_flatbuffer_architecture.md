# C6 <-> S3 Архитектура (фиксировано)

Дата: 2026-02-10

## Границы ответственности

- `ESP32-C6`
  - Zigbee stack и Zigbee runtime.
  - Источник правды по устройствам Zigbee.
  - Хранение device snapshot в виде FlatBuffer (RAM + persist).
  - Передача device FlatBuffer на S3 по UART без трансформаций.

- `ESP32-S3`
  - UI (LCD/LVGL), HTTP/WS, web frontend.
  - Хранение automations FlatBuffer.
  - Runtime hash-индекс автоматизаций для быстрого поиска кандидатов по входящему событию.
  - Полученный от C6 device FlatBuffer сохраняется как есть и отдается фронту без изменений.

## Нефункциональные инварианты

- JSON в транспортном контуре не используется.
- Realtime события между MCU остаются бинарными.
- Для device sync используется passthrough буфер FlatBuffer: `C6 -> UART -> S3 -> frontend`.
- S3 не пересобирает и не нормализует device FlatBuffer при проксировании на frontend.

## Что меняем сейчас

1. `S3 rules_engine`: добавить hash-индекс триггеров автоматизаций.
2. `S3 automations`: индекс обновляется при save/remove/enable/disable.
3. `Device sync`: подготовить и внедрить контракт full-sync + update по FlatBuffer буферу.

## Алгоритм быстрого поиска автоматизаций

1. На reload кэша автоматизаций построить hash-таблицу триггеров.
2. Ключ индекса:
   - event_type,
   - optional device_uid,
   - optional endpoint,
   - optional cmd/cluster/attr (по типу события),
   - маска присутствия полей.
3. На входящее событие:
   - сгенерировать комбинации ключей (exact + wildcard),
   - получить candidate set из индекса,
   - выполнить точную проверку trigger + conditions только для кандидатов,
   - выполнить actions.

## План по FlatBuffer sync устройств

- Этап A: добавить transport messages для device FlatBuffer snapshot/update.
- Этап B: на C6 сформировать/обновлять device FlatBuffer буфер и передавать chunk-ами.
- Этап C: на S3 принимать chunk-ы, собирать буфер, сохранять как есть, отдавать frontend raw binary.
- Этап D: добавить version/seq и защиту от частичных обновлений (atomic swap буфера).

## Критерий готовности

- При потоке Zigbee событий S3 не сканирует весь список правил.
- Device list на frontend обновляется из сырого FlatBuffer буфера C6 без преобразования.
- После рестарта S3 состояние девайсов восстанавливается из последнего валидного FlatBuffer snapshot.
