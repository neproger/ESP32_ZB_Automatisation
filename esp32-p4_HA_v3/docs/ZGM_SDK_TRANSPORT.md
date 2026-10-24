# SDK transport audit и модель Request Executor

Дата: 24.10.2026. Дополнение к `Z2M_AUDIT.md`. Источники — заголовки
`esp-zigbee-lib` из managed-компонента (IDF 6.1) и исходники herdsman 11.1.1.

Цель: не писать свой Zigbee-стек и не дублировать то, что уже делает SDK.
Перенять у Herdsman **модель надёжного взаимодействия**, насколько её позволяет
Espressif API.

## 1. Что даёт Espressif SDK

### 1.1. Запросы ZCL

`ezbee/zcl/zcl_general_cmd.h`:

- `ezb_zcl_read_attr_cmd_req`
- `ezb_zcl_write_attr_cmd_req`
- `ezb_zcl_config_report_cmd_req`
- `ezb_zcl_read_report_config_cmd_req`
- `ezb_zcl_report_attr_cmd_req`
- `ezb_zcl_default_rsp_cmd_req`

Общий `ezb_zcl_cmd_ctrl_t` (`zcl_common.h:44`): `dst_addr`, `dst_ep`, `src_ep`,
`cluster_id`, `manuf_code`, `fc` и `cnf_ctx`. **TSN в запросе нет** — его
назначает SDK. Подтверждение приходит в callback из `cnf_ctx`:

`ezb_af_user_cnf_t` (`af.h:213`):
`status`, **`tsn`**, `dst_addr`, `src_ep`, `dst_ep`, `cluster_id`, `profile_id`.

То есть после отправки мы узнаём TSN и статус передачи, но не сам ответ устройства.

### 1.2. Ответы устройства

Ответы/отчёты приходят через core action callbacks (`zcl_core.h`):

- `EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID`
- `EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID`
- `EZB_ZCL_CORE_REPORT_ATTR_CB_ID` (уже используется)
- `EZB_ZCL_CORE_DEFAULT_RSP_CB_ID` — `rsp_to_cmd`, `status_code`

Каждое сообщение содержит `header` с `tsn`, `src_addr`, `src_ep`, `cluster_id`,
`cmd_id`, `rssi`. Это и есть база для waiters, но **контекст запроса сюда не
передаётся** — сопоставление делает приложение по TSN и адресу.

`ezb_zcl_cmd_report_attr_message_t` отдаёт список разобранных атрибутов (один
кадр — N атрибутов); граница C→Rust уже реализована.

### 1.3. ZDO Bind/Unbind

`ezb_zdo_bind_req_t` (`zdo_bind_mgmt.h`): `dst_nwk_addr`, `field`, `cb`,
`user_ctx`. Результат — `ezb_zdp_bind_req_result_t`, callback получает его
синхронно по завершении. **SDK сам коррелирует ZDO-запрос и ответ**; строить для
ZDO отдельные TSN-waiters не нужно, достаточно дождаться callback'а с `user_ctx`.

### 1.4. APS confirm

`ezb_apsde_data_confirm_t` (`aps.h`): `status`, адреса/endpoint'ы, cluster,
profile, `asdu`. TSN в структуре нет, но ASDU содержит ZCL-заголовок — при
необходимости можно разобрать. Для ZCL-уровня обычно достаточно AF/ZCL
`cnf_ctx`.

### 1.5. Блокировка

Любой вызов SDK вне Zigbee callbacks требует `esp_zigbee_lock_acquire`
(`esp_zigbee.h:197`). Значит executor, живущий в отдельной Rust-задаче, обязан
брать lock на каждый запрос; из callback'ов — нельзя (иначе дедлок).

### 1.6. Чего в SDK нет (по заголовкам/Kconfig)

- нет API per-endpoint очереди и send policies (`bulk/immediate/...`);
- нет request expiry;
- нет явных настроек APS/NWK retry/timeout (Kconfig содержит только debug);
- нет подтверждения **фактического состояния** — только транспорт/ZCL.

Следствие: внутренние повторы SDK для APS/MAC/NWK непрозрачны. Приложение не
должно их дублировать «на всякий случай».

## 2. Сопоставление Herdsman ↔ SDK

| Herdsman | SDK | Что добавляем мы |
|---|---|---|
| TSN назначается helper'ом | SDK назначает, TSN приходит в confirm | храним TSN в pending-op |
| waiter по `(addr, ep, cluster, cmd, tsn)` | core action response с header.tsn | таблица waiters + учёт гонки |
| data confirm + recoverable retry (4, backoff 2 s) | AF/APS confirm, внутренние повторы неизвестны | app-retry только по отсутствию confirm/response, ограниченно |
| per-endpoint request queue, expiry, send policies | нет | executor: очередь, deadline, in-flight=1 на endpoint |
| ZDO bind через waiter | SDK callback с `user_ctx` | ждём callback, TSN не нужен |
| configure: codepath + scheduler | нет | z2m-rs строит ops; HA Layer планирует |
| reporting: bind/read/config report | готовые `*_cmd_req` | ops + ожидание ответов |
| sleepy: конфиг по пробуждению | NWK может буферизовать, API нет | парковка op до следующего события устройства |
| availability | нет | HA Layer |

## 3. Модель операции в HA V3

Три независимых состояния (не смешивать):

```text
Desired state      — чего хочет пользователь/автоматизация (Domain/UI)
Operation status   — доставка и протокольное подтверждение (executor)
Reported state     — что устройство реально сообщило (Domain facts)
```

Статусы операции (RAM, компактно, без MicroDB):

```text
Queued -> Sent -> Acknowledged -> Confirmed
                    \-> Timeout/Failed/Expired
```

- `Sent` — SDK принял запрос;
- `Acknowledged` — пришёл AF confirm со статусом success (доставка);
- `Confirmed` — получен требуемый ответ/чтение/отчёт (критерий задаёт op);
- `Timeout/Failed/Expired` — исходы; `Expired` не исполняем вовсе (устаревший desired).

Не все операции доходят до `Confirmed`: у команд без ответа критерием может быть
`Acknowledged`. `Confirmed` — это не «пользователь увидел ON», а «получено
протокольное подтверждение согласно типу операции». Фактическое состояние —
только из отчётов/чтения.

Optimistic-режим Z2M не копируем: UI может показывать desired, Domain — только
подтверждённое.

## 4. Correlation

Ключ ожидания: `(dst eui64 или nwk, dst_ep, cluster, expected cmd, tsn)`.
Дополнительно `defaultRsp` матчится по `rsp_to_cmd` (как в Herdsman).

Гонка: ответ устройства теоретически может прийти раньше AF confirm (TSN ещё
неизвестен приложению). Решение:

1. при отправке регистрируем pending-op по `user_ctx` (id операции);
2. AF confirm приносит `tsn` → связываем с op;
3. core action response без совпавшего waiter кладём в короткий буфер
   `(tsn, src, ep, cluster, cmd)`; при confirm пробуем снова;
4. буфер ограничен и чистится по времени.

ZDO Bind: pending-op ждёт callback SDK по `user_ctx`; отдельного TSN-пути нет.

## 5. Очередь, retry, expiry

- Один in-flight на `(device, endpoint)`; остальные op ждут (как RequestQueue).
- Deadline у каждой op; просроченные отклоняются с `Expired` и не отправляются.
- `Queued` ограничена; переполнение — явная ошибка/счётчик, не молчаливая потеря.
- Retry политика:
  - повторяем только при отсутствии AF confirm / отсутствии ожидаемого ответа /
    явном failure-статусе;
  - максимум 2–3 попытки, backoff;
  - **не повторяем неидемпотентные команды** (toggle, scene, step) без явного
    флага; on/off/read/bind/reporting идемпотентны;
  - внутренние повторы SDK не дублируем.
- Sleepy: при timeout для EndDevice op паркуется до следующего события от
  устройства (report/annce/last-seen), максимум N попыток. Очередь других
  устройств при этом не блокируется.

## 6. Слои

```text
z2m-rs        : Command/ConfigurePlan -> ZCL request data + expected response kind
zigbee-esp    : lock, send, TSN, waiters, queue, retry, expiry, ZDO callback
HA Zigbee Layer: scheduling, single-flight configure, configured version, status
Domain        : desired/reported facts через канонические типы
```

`z2m-rs` не знает SDK и не держит TSN. `zigbee-esp` не знает устройств.
HA Layer — единственное место преобразования в canonical-типы.

## 7. Request Executor: минимальный контракт (для реализации)

```rust
enum OpKind {
    Zcl { cluster, cmd, payload, expect: Expect },
    ZdoBind { ... },
}
enum Expect { None, ZclResponse(u8), DefaultResponse(u8), Readback { .. } }

trait Transport {
    fn send(&mut self, req: &Request) -> Result<Tsn, SendError>;
    fn poll(&mut self) -> Option<Event>; // confirm / response / timeout
}

// Executor хранит очередь per-endpoint, pending по tsn/user_ctx,deadline,
// attempts, статус; наружу отдаёт OperationEvent { id, status, result }.
```

Fake transport для host-тестов: управляемые TSN, confirm, response, таймауты,
потеря ответа, гонка response-before-confirm, expiry, ZDO callback.

## 8. План

1. Fake transport + executor: TSN/waiter/гонка, queue, expiry, retry, статусы.
2. SDK adapter: lock, `*_cmd_req`, AF confirm, core action responses,
   ZDO bind callback.
3. ConfigurePlan executor: Bind (ZDO), Read, ConfigureReporting + шаги.
4. Sleepy-планировщик; затем Interview (manufacturer/model → профиль).
5. Только после этого — масштабирование каталога.

## 9. Открытые вопросы

- Внутренние retry/timeout SDK не документированы; проверить экспериментально.
- Статусы AF confirm и их соответствие Z2M-ошибкам.
- Возможна ли гонка response-before-confirm на практике (буфер всё равно нужен).
- Максимум in-flight на устройство/endpoint у SDK.
- Поведение SDK при отправке спящему устройству (буферизация NWK?).
- Явные коды ошибок для `Failed` (transport vs ZCL status vs decode).
