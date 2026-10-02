#pragma once

#include <stddef.h>

#include "domain/domain_event.h"
#include "domain/domain_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Domain — инфраструктурное ядро фактов: хранит, журналирует, доставляет.
 * Семантика принадлежит сервисам (docs/ARCHITECTURE.md §3).
 */
typedef struct {
    void *_state;
} domain_t;

/*
 * Поднимает Domain и его registry типов. max_entity_types — максимальное число
 * зарегистрированных типов сущностей; ёмкость самих таблиц задаётся в descriptor.
 * payload_capacity / payload_max_size — bounded ring под transient payload: сколько
 * payload одновременно живы и какой максимальный размер одного (§TRANSIENT_PAYLOAD.md).
 */
sys_error_t domain_init(domain_t *domain, size_t max_entity_types, size_t journal_capacity,
                        size_t payload_capacity, size_t payload_max_size);
sys_error_t domain_deinit(domain_t *domain);

/*
 * Регистрирует тип сущности и поднимает под него таблицу хранилища.
 * Вызывается из bootstrap приложения (docs/RECORD_MODEL.md §1.1), а не из сервиса.
 */
sys_error_t domain_register_entity(domain_t *domain, const domain_entity_desc_t *desc);

/*
 * Обход записей типа. Указатели key/record действительны только во время вызова и
 * не должны сохраняться; мутирующий API Domain внутри колбэка вызывать нельзя
 * (docs/domain/DOMAIN_API.md §8-9). Возврат false прерывает обход.
 */
typedef bool (*domain_entity_iter_cb_t)(const void *key, const void *record, void *ctx);

/*
 * Компактное описание факта от того, кто меняет состояние: Domain не «догадывается»,
 * что писать в Journal, а получает это от вызывающего (ENTITY_STORE.md §4).
 * ts / event_id / entity / key / op добавляет сам Domain.
 *
 * Ссылки на payload здесь нет: её выдаёт ring при domain_payload_put, а не вызывающий,
 * поэтому ref/size факта задаёт только сам put.
 */
typedef struct {
    uint8_t source;
    domain_value_t value;
} domain_fact_meta_t;

/*
 * Адресат факта для команд и событий: к какой сущности факт относится. Операции над
 * сущностями адресат не передают — он известен из самой операции (type + key).
 *
 * key — по layout'у типа сущности; размер ключа Domain берёт из descriptor'а, поэтому
 * передавать его не нужно (docs/domain/COMMANDS.md §8). Адресат может отсутствовать:
 * тогда факт пишется без entity/key.
 */
typedef struct {
    domain_entity_t entity;
    const void *key;
} domain_fact_target_t;

/*
 * Запись сущности. Размеры key и record заданы descriptor'ом типа.
 * out_changed обязателен: «записали» и «ничего не изменилось» — разные исходы.
 *
 * Факт пишется по исходу операции (JOURNAL.md §2.1): при реальном изменении —
 * ENTITY_UPSERTED, при runtime-ошибке — ERROR. Результат операции возвращается
 * вызывающему тем же кодом: Journal — параллельный след, а не проверка. В штатном
 * пути append не отказывает, поэтому его ошибка — нарушение внутреннего контракта.
 */
sys_error_t domain_entity_put(domain_t *domain, domain_entity_t type,
                               const void *key, const void *record,
                               const domain_fact_meta_t *meta, bool *out_changed);
sys_error_t domain_entity_get(domain_t *domain, domain_entity_t type,
                               const void *key, void *out_record);
sys_error_t domain_entity_remove(domain_t *domain, domain_entity_t type, const void *key,
                                  const domain_fact_meta_t *meta);
sys_error_t domain_entity_iter(domain_t *domain, domain_entity_t type,
                                domain_entity_iter_cb_t cb, void *ctx);

/*
 * Transient payload: best-effort окно для данных, которым тесно в compact value.
 *
 * put копирует payload в ring и публикует факт EVENT с payload_ref и payload_size —
 * это и есть «опубликовать событие с данными» (docs/domain/TRANSIENT_PAYLOAD.md §2-3).
 * get отдаёт payload в буфер вызывающего: размер берётся из факта (payload_size), чтобы
 * не гадать о длине. Вытесненный payload читается как STALE, а неизвестная ссылка — как
 * NOT_FOUND. Никакого ownership и release: payload живёт, пока его не вытеснит ring.
 */
sys_error_t domain_payload_put(domain_t *domain, const domain_fact_target_t *target,
                               const domain_fact_meta_t *meta, const void *payload, size_t size,
                               domain_payload_ref_t *out_ref);
sys_error_t domain_payload_get(domain_t *domain, domain_payload_ref_t ref, void *out,
                               size_t out_size);

/*
 * Регистрация исполнителя команды: command_type → callback. Это часть инициализации,
 * а не runtime-механизма (docs/domain/COMMANDS.md §3).
 */
typedef sys_error_t (*domain_command_fn)(domain_command_t type, const void *args,
                                         size_t args_size, void *ctx);

sys_error_t domain_register_command(domain_t *domain, domain_command_t type,
                                    domain_command_fn executor, void *ctx);

/*
 * Передаёт команду исполнителю синхронно: вызов возвращается, когда команда передана.
 * Результат выполнения не ожидается (fire-and-forget). При успешной передаче пишется
 * COMMAND_SENT; если executor не найден или отклонил команду — возвращается его ошибка
 * и факта нет (docs/domain/COMMANDS.md §2).
 *
 * target — адресат факта: чтобы подписчик мог отнести команду к устройству. Адресат
 * проверяется до вызова executor'а: неизвестный тип — отказ, факта нет.
 */
sys_error_t domain_post(domain_t *domain, domain_command_t type, const void *args, size_t args_size,
                        const domain_fact_target_t *target, const domain_fact_meta_t *meta);

/*
 * Подписка на факты. Фильтр задаётся при подписке и применяется Dispatcher'ом
 * (DISPATCHER.md §5): маска 0 / entity 0 — «любые».
 *
 * try_push вызывается в контексте Dispatcher'а и обязан быть коротким: он только
 * кладёт событие в inbox подписчика. false означает «inbox полон» — это локальная
 * потеря сервиса, Dispatcher продолжает. wake будит задачу подписчика.
 * Внутри try_push/wake нельзя вызывать API Domain.
 */
typedef struct {
    uint32_t kind_mask;
    uint32_t source_mask;
    domain_entity_t entity;
    bool (*try_push)(const domain_event_t *event, void *ctx);
    void (*wake)(void *ctx);
    void *ctx;
} domain_subscription_desc_t;

typedef struct domain_subscription domain_subscription_t;

sys_error_t domain_subscribe(domain_t *domain, const domain_subscription_desc_t *desc,
                              domain_subscription_t **out_sub);
sys_error_t domain_unsubscribe(domain_t *domain, domain_subscription_t *sub);

/*
 * Доставка: читает Journal от cursor до newest и раскладывает по подписчикам.
 * Вызывается задачей Dispatcher'а; возвращает число доставленных событий.
 * domain_dispatch_wait блокирует до появления нового факта (сигнал, не polling).
 */
sys_error_t domain_dispatch_once(domain_t *domain, size_t *out_delivered);
sys_error_t domain_dispatch_wait(domain_t *domain, uint32_t timeout_ms, bool *out_signalled);

#ifdef __cplusplus
}
#endif
