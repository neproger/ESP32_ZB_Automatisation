#pragma once

#include <stdbool.h>

#include "domain/domain.h"
#include "domain/domain_types.h"
#include "domain_journal.h"
#include "mstore/mstore_table.h"
#include "mstore/mstore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool used;
    domain_entity_desc_t desc;
    mstore_table_t table;

    /*
     * scratch под key: mstore отдаёт meta + key + payload одним snapshot'ом,
     * а чтению сущности ключ не нужен. Буфер на тип, а не на вызов: аллокаций
     * в read-path быть не должно.
     */
    void *scratch_key;
} domain_entity_entry_t;

/*
 * Transient payload — bounded ring поверх Ring Store. Запись одна на payload и
 * фиксированного размера (размер + байты), поэтому ring вытесняет целиком.
 */
typedef struct {
    mstore_ring_t ring;
    size_t max_size;
    void *scratch; /* буфер одной записи: put/get не аллоцируют */
} domain_payload_t;

struct domain_command_entry {
    domain_command_t type;
    domain_command_fn executor;
    void *ctx;
    struct domain_command_entry *next;
};

typedef struct {
    domain_entity_entry_t *entries;
    size_t used;
    size_t capacity;

    /*
     * Сериализует весь mutation path и read path: порядок «запись + факт»,
     * доступ к scratch_key и обход. Lock не рекурсивный — колбэк iter не имеет
     * права вызывать API Domain (docs/domain/DOMAIN_API.md §9).
     */
    void *lock;

    domain_journal_t journal;

    /* Transient payload: отдельный bounded ring, живёт по тем же правилам вытеснения. */
    domain_payload_t payload;

    /*
     * Регистрация команд: command_type → executor. Список, а не таблица: команд мало,
     * а регистрация — часть инициализации, поэтому ёмкость задавать нечем.
     */
    struct domain_command_entry *commands;

    /*
     * Dispatcher: подписки, курсор чтения Journal и сигнал пробуждения.
     * Cursor — следующий ожидаемый event_id; gap фиксируется, если курсор ушёл
     * левее oldest (JOURNAL.md §5).
     */
    struct domain_subscription *subscriptions;
    domain_event_id_t cursor;
    uint32_t gap_count;
    void *signal;

    /*
     * Отдельный lock доставки: защищает список подписок и курсор Journal.
     * Общий Domain-lock здесь не используется: подписчик не должен блокировать
     * Entity Store (DISPATCHER.md §5).
     */
    void *dispatch_lock;
} domain_state_t;

/*
 * Domain создаёт свою ошибку только тогда, когда она возникла в нём. Ошибку ниже-
 * лежащего слоя он передаёт как есть (docs/ERRORS.md §1).
 */
static inline sys_error_t domain_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_DOMAIN, code);
}

/*
 * Сборка и запись факта — единственное место, где факт обретает форму. Операции
 * (entity / payload / command) решают, что писать, но не как (JOURNAL.md §2.2).
 */
void domain_fact_write(domain_state_t *state, const domain_fact_meta_t *meta,
                       domain_entity_t type, const void *key, uint8_t key_size, uint8_t kind,
                       uint8_t op, sys_error_t error, uint64_t payload_ref);

domain_state_t *domain_state(const domain_t *domain);
domain_entity_entry_t *domain_entry_find(domain_state_t *state, domain_entity_t type);

/* Transient payload (src/domain_payload.c). */
sys_error_t domain_payload_init(domain_payload_t *payload, size_t capacity, size_t max_size);
void domain_payload_deinit(domain_payload_t *payload);

/* Реестр команд (src/domain_command.c). Поиск идёт под lock'ом вызывающего. */
struct domain_command_entry *domain_command_find(domain_state_t *state, domain_command_t type);
void domain_commands_deinit(domain_state_t *state);

/* Подписки и доставка (src/domain_dispatch.c). */
sys_error_t domain_dispatch_init(domain_state_t *state);
void domain_dispatch_deinit(domain_state_t *state);
void domain_dispatch_signal(domain_state_t *state);

#ifdef __cplusplus
}
#endif
