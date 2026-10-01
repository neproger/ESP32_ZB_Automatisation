#include "domain/domain.h"

#include <string.h>

#include "domain_internal.h"
#include "domain_platform.h"
#include "mstore/mstore_table.h"

domain_err_t domain_err_from_mstore(mstore_err_t err)
{
    switch (err) {
    case MSTORE_OK:
        return DOMAIN_OK;
    case MSTORE_NOT_FOUND:
        return DOMAIN_NOT_FOUND;
    case MSTORE_STALE:
        return DOMAIN_STALE;
    case MSTORE_NO_SPACE:
        return DOMAIN_NO_SPACE;
    case MSTORE_INVALID_ARG:
        return DOMAIN_INVALID_ARG;
    case MSTORE_INVALID_SIZE:
        return DOMAIN_INVALID_SIZE;
    case MSTORE_INVALID_STATE:
        return DOMAIN_INVALID_STATE;
    case MSTORE_NO_MEM:
        return DOMAIN_NO_MEM;
    case MSTORE_INVARIANT_FAILED:
        return DOMAIN_CORRUPT;
    case MSTORE_IO:
        return DOMAIN_IO;
    case MSTORE_CORRUPT:
        return DOMAIN_CORRUPT;
    case MSTORE_OVERFLOW:
        return DOMAIN_OVERFLOW;
    default:
        return DOMAIN_INVALID_STATE;
    }
}

domain_state_t *domain_state(const domain_t *domain)
{
    if (domain == NULL) {
        return NULL;
    }
    return (domain_state_t *)domain->_state;
}

domain_entity_entry_t *domain_entry_find(domain_state_t *state, domain_entity_t type)
{
    if (state == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < state->used; ++i) {
        if (state->entries[i].used && state->entries[i].desc.type == type) {
            return &state->entries[i];
        }
    }
    return NULL;
}

/*
 * Проверяет только то, чего не проверяет mstore: capacity / key_size / backing он
 * валидирует сам и вернёт свой код, который мы маппим. persist_key при FLASH оставлен
 * здесь намеренно — это требование контракта Domain (RECORD_MODEL.md §3), а не storage.
 */
static domain_err_t desc_validate(const domain_entity_desc_t *desc)
{
    if (desc->payload_size == 0) {
        return DOMAIN_INVALID_SIZE;
    }
    /* Ключ копируется в запись Journal целиком, поэтому шире лимита он быть не может. */
    if (desc->key_size > DOMAIN_JOURNAL_KEY_MAX) {
        return DOMAIN_INVALID_SIZE;
    }
    if ((desc->backing & DOMAIN_BACKING_FLASH) != 0 && desc->persist_key == NULL) {
        return DOMAIN_INVALID_ARG;
    }
    return DOMAIN_OK;
}

static mstore_backing_t backing_to_mstore(domain_backing_t backing)
{
    mstore_backing_t result = MSTORE_BACKING_NONE;
    if ((backing & DOMAIN_BACKING_RAM) != 0) {
        result |= MSTORE_BACKING_RAM;
    }
    if ((backing & DOMAIN_BACKING_FLASH) != 0) {
        result |= MSTORE_BACKING_FLASH;
    }
    return result;
}

domain_err_t domain_init(domain_t *domain, size_t max_entity_types, size_t journal_capacity)
{
    if (domain == NULL || max_entity_types == 0 || journal_capacity == 0) {
        return DOMAIN_INVALID_ARG;
    }
    if (domain->_state != NULL) {
        return DOMAIN_INVALID_STATE;
    }

    domain_state_t *state = domain_platform_alloc(sizeof(*state));
    if (state == NULL) {
        return DOMAIN_NO_MEM;
    }
    memset(state, 0, sizeof(*state));

    state->entries = domain_platform_alloc(max_entity_types * sizeof(*state->entries));
    if (state->entries == NULL) {
        domain_platform_free(state);
        return DOMAIN_NO_MEM;
    }
    memset(state->entries, 0, max_entity_types * sizeof(*state->entries));

    state->lock = domain_platform_lock_create();
    if (state->lock == NULL) {
        domain_platform_free(state->entries);
        domain_platform_free(state);
        return DOMAIN_NO_MEM;
    }
    const domain_err_t journal_err = domain_journal_init(&state->journal, journal_capacity);
    if (journal_err != DOMAIN_OK) {
        domain_platform_lock_destroy(state->lock);
        domain_platform_free(state->entries);
        domain_platform_free(state);
        return journal_err;
    }

    state->capacity = max_entity_types;

    domain->_state = state;
    return DOMAIN_OK;
}

domain_err_t domain_deinit(domain_t *domain)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL) {
        return DOMAIN_INVALID_STATE;
    }

    (void)domain_journal_deinit(&state->journal);
    for (size_t i = 0; i < state->used; ++i) {
        if (state->entries[i].used) {
            (void)mstore_table_deinit(&state->entries[i].table);
            domain_platform_free(state->entries[i].scratch_key);
        }
    }
    domain_platform_free(state->entries);
    domain_platform_lock_destroy(state->lock);
    domain_platform_free(state);
    domain->_state = NULL;
    return DOMAIN_OK;
}

domain_err_t domain_register_entity(domain_t *domain, const domain_entity_desc_t *desc)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || desc == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    const domain_err_t checked = desc_validate(desc);
    if (checked != DOMAIN_OK) {
        return checked;
    }
    mstore_table_schema_t schema = {0};
    schema.capacity = desc->capacity;
    schema.key_size = desc->key_size;
    schema.payload_size = desc->payload_size;
    schema.payload_equals = NULL;
    schema.backing = backing_to_mstore(desc->backing);
    schema.persist_key = desc->persist_key;

    domain_platform_lock_acquire(state->lock);
    domain_err_t result = DOMAIN_OK;
    if (domain_entry_find(state, desc->type) != NULL) {
        result = DOMAIN_INVALID_STATE;
    } else if (state->used == state->capacity) {
        result = DOMAIN_NO_SPACE;
    }
    domain_platform_lock_release(state->lock);
    if (result != DOMAIN_OK) {
        return result;
    }

    void *scratch_key = domain_platform_alloc(desc->key_size);
    if (scratch_key == NULL) {
        return DOMAIN_NO_MEM;
    }

    mstore_table_t table = {0};
    const mstore_err_t err = mstore_table_init(&table, &schema);
    if (err != MSTORE_OK) {
        domain_platform_free(scratch_key);
        return domain_err_from_mstore(err);
    }

    domain_platform_lock_acquire(state->lock);
    if (domain_entry_find(state, desc->type) != NULL || state->used == state->capacity) {
        domain_platform_lock_release(state->lock);
        (void)mstore_table_deinit(&table);
        domain_platform_free(scratch_key);
        return DOMAIN_INVALID_STATE;
    }

    domain_entity_entry_t *entry = &state->entries[state->used];
    entry->desc = *desc;
    entry->table = table;
    entry->scratch_key = scratch_key;
    entry->used = true;
    state->used++;
    domain_platform_lock_release(state->lock);
    return DOMAIN_OK;
}
