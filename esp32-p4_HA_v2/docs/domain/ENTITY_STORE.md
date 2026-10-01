# Entity Store

Слой: **Domain / Entity Store**. Карта системы — `../ARCHITECTURE.md`.
Дисциплина записей — `../RECORD_MODEL.md`.
Документ описывает контракт и поведение; конкретная реализация — в коде.

## 1. Роль

Entity Store хранит **последнее известное состояние** системы. Это единственный
источник истины (не Journal).

Все entity **равны**: `device`, `endpoint`, `device_state`, `group`, `group_item`,
`automation`, `settings`. Различаются только payload.

## 2. Registry

Простая routing-таблица, собранная при регистрации типов:

```text
entity_type → { таблица/schema, optional validate }
```

Состав таблиц, их ёмкость и backing задаёт bootstrap приложения через
`domain_register_entity()`; Entity Store хранит descriptors и не знает форм данных
(`../RECORD_MODEL.md` §1.1).

- Registry не framework; никаких pre/post hooks, стадий коммита и schema-хуков для
  Journal.
- `entity_type` нужен только чтобы выбрать хранилище.
- Если у типа есть обязательный invariant — допускается validator.

## 3. API

```text
get(entity, key)
list(entity, filter?)
upsert(entity, key, record, event_meta)
remove(entity, key, event_meta)
```

- Все entity проходят один жизненный цикл.
- Большие payload (например automation на несколько КБ) живут здесь и в Journal
  не копируются.

## 4. Кто описывает факт

Domain не открывает запись, чтобы «догадаться», что писать в Journal. Тот, кто меняет
состояние, передаёт компактное описание факта:

```text
event_meta = { source, value, payload_ref? }
```

Domain добавляет `event_id / ts / entity / key / op` и не вычисляет, почему значение
важно.

## 5. Ключи и связи

Domain не знает отношений между сущностями. Связи выражаются данными:

- составные ключи: `endpoint = (device_uid, endpoint)`;
- ссылки в полях: `endpoint.device_uid`, `group_item.device_uid/endpoint`;
- списки внутри записи (например `endpoint.in_clusters[]`).

Примеры: «endpoint'ы устройства» → `list(ENDPOINT, prefix=device_uid)`; «родитель
кластера» → endpoint, в котором кластер лежит. Relationship-таблиц нет.

## 6. Remove

- Факт удаления (`ENTITY_REMOVED`) несёт полный canonical key — читать запись уже
  нельзя.
- Каскадное удаление — последовательность отдельных `remove`/событий. Атомарность
  не вводим, пока не появится конкретный invariant, требующий её.

## 7. Порядок и конкурентность

- Запись store и запись факта в Journal имеют один сериализованный порядок.
- Last-writer-wins; конфликт-резолюшн и merge не нужны.
- Физический update/version — только если snapshot реально изменился (это решение
  storage, не store).
- Read API берёт только внутреннюю защиту таблицы хранилища.
