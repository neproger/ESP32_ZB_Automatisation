#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/domain_types.h"
#include "mstore/mstore_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Journal — внутренняя подсистема Domain: поток фактов поверх Ring Store.
 * Сервисы журнал не читают: единственный потребитель — Dispatcher
 * (docs/domain/JOURNAL.md §5, DISPATCHER.md §3).
 */

typedef uint64_t domain_event_id_t;

/* Ключ копируется в запись целиком; это ограничивает размер ключа типа. */
#define DOMAIN_JOURNAL_KEY_MAX 24

typedef enum {
    DOMAIN_FACT_ENTITY_UPSERTED = 1,
    DOMAIN_FACT_ENTITY_REMOVED = 2,
    DOMAIN_FACT_EVENT = 3,
    DOMAIN_FACT_COMMAND_SENT = 4,
} domain_fact_kind_t;

typedef enum {
    DOMAIN_OP_UPSERT = 1,
    DOMAIN_OP_REMOVE = 2,
} domain_op_t;

typedef enum {
    DOMAIN_SOURCE_ZIGBEE = 1,
    DOMAIN_SOURCE_UI = 2,
    DOMAIN_SOURCE_AUTOMATION = 3,
    DOMAIN_SOURCE_SYSTEM = 4,
} domain_source_t;

typedef enum {
    DOMAIN_VALUE_NONE = 0,
    DOMAIN_VALUE_BOOL = 1,
    DOMAIN_VALUE_I32 = 2,
    DOMAIN_VALUE_U32 = 3,
    DOMAIN_VALUE_F32 = 4,
    DOMAIN_VALUE_ENUM = 5,
} domain_value_type_t;

typedef struct {
    uint8_t type;
    uint8_t reserved[3];
    union {
        uint32_t u32;
        int32_t i32;
        float f32;
    } v;
} domain_value_t;

/*
 * Запись Journal. event_id назначается самим Journal'ем: это seq Ring Store, поэтому
 * внутри ring поле смысла не несёт (identity факта и есть seq, MSTORE.md §5) — Journal
 * проставляет его в копию вызывающего при append и при чтении.
 * Ключ копируется целиком: факт удаления обязан нести полный canonical key, потому что
 * читать запись уже нельзя (ENTITY_STORE.md §6).
 */
typedef struct {
    uint64_t event_id;
    uint64_t ts;
    uint8_t kind;
    uint8_t op;
    uint8_t source;
    uint8_t key_size;
    uint32_t entity;
    uint8_t key[DOMAIN_JOURNAL_KEY_MAX];
    domain_value_t value;
    uint64_t payload_ref;
} domain_event_t;

#ifdef __cplusplus
static_assert(sizeof(domain_event_t) <= 128, "domain_event_t слишком большой для ring");
#else
_Static_assert(sizeof(domain_event_t) <= 128, "domain_event_t слишком большой для ring");
#endif

typedef struct {
    mstore_ring_t ring;
} domain_journal_t;

domain_err_t domain_journal_init(domain_journal_t *journal, size_t capacity);
domain_err_t domain_journal_deinit(domain_journal_t *journal);

domain_err_t domain_journal_append(domain_journal_t *journal, domain_event_t *event,
                                   domain_event_id_t *out_id);
domain_err_t domain_journal_get(const domain_journal_t *journal, domain_event_id_t id,
                                domain_event_t *out);

domain_err_t domain_journal_oldest(const domain_journal_t *journal, domain_event_id_t *out);
domain_err_t domain_journal_newest(const domain_journal_t *journal, domain_event_id_t *out);
domain_err_t domain_journal_count(const domain_journal_t *journal, size_t *out);
domain_err_t domain_journal_contains(const domain_journal_t *journal, domain_event_id_t id,
                                     bool *out);

#ifdef __cplusplus
}
#endif
