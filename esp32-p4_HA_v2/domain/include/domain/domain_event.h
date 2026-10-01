#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/domain_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Запись факта Journal. Публичная, потому что её получает подписчик: Dispatcher
 * доставляет именно её (DISPATCHER.md §2), отдельного event-lookup нет.
 */

typedef uint64_t domain_event_id_t;

/* Ключ копируется в запись целиком; это ограничивает размер ключа типа. */
#define DOMAIN_EVENT_KEY_MAX 24

typedef enum {
    DOMAIN_FACT_ENTITY_UPSERTED = 1,
    DOMAIN_FACT_ENTITY_REMOVED = 2,
    DOMAIN_FACT_EVENT = 3,
    DOMAIN_FACT_COMMAND_SENT = 4,
    DOMAIN_FACT_ERROR = 5,
} domain_fact_kind_t;

typedef enum {
    DOMAIN_OP_ENTITY_PUT = 1,
    DOMAIN_OP_ENTITY_REMOVE = 2,
    DOMAIN_OP_COMMAND = 3,
    DOMAIN_OP_PAYLOAD_PUT = 4,
    DOMAIN_OP_ENTITY_ITER = 5,
} domain_op_t;

typedef struct {
    uint64_t event_id;
    uint64_t ts;
    uint8_t kind;
    uint8_t op;
    uint8_t source;
    uint8_t key_size;
    uint32_t entity;
    uint8_t key[DOMAIN_EVENT_KEY_MAX];
    domain_value_t value;
    uint64_t payload_ref;

    /* Только для kind = ERROR: код результата операции. */
    uint32_t error;
} domain_event_t;

#ifdef __cplusplus
static_assert(sizeof(domain_event_t) <= 128, "domain_event_t слишком большой для ring");
#else
_Static_assert(sizeof(domain_event_t) <= 128, "domain_event_t слишком большой для ring");
#endif

#ifdef __cplusplus
}
#endif
