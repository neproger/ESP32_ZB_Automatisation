#include "domain/domain.h"

#include <string.h>

#include "domain_internal.h"
#include "domain_platform.h"
#include "mstore/mstore_table.h"

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
static sys_error_t desc_validate(const domain_entity_desc_t *desc)
{
    if (desc->payload_size == 0) {
        return domain_fail(SYS_CODE_INVALID_SIZE);
    }
    /* Ключ копируется в запись факта целиком, поэтому шире лимита он быть не может. */
    if (desc->key_size > DOMAIN_EVENT_KEY_MAX) {
        return domain_fail(SYS_CODE_INVALID_SIZE);
    }
    if ((desc->backing & DOMAIN_BACKING_FLASH) != 0 && desc->persist_key == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    return SYS_OK;
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

sys_error_t domain_init(domain_t *domain, size_t max_entity_types, size_t journal_capacity,
                        size_t payload_capacity, size_t payload_max_size)
{
    if (domain == NULL || max_entity_types == 0 || journal_capacity == 0 ||
        payload_capacity == 0 || payload_max_size == 0) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    if (domain->_state != NULL) {
        return domain_fail(SYS_CODE_INVALID_STATE);
    }

    domain_state_t *state = domain_platform_alloc(sizeof(*state));
    if (state == NULL) {
        return domain_fail(SYS_CODE_NO_MEM);
    }
    memset(state, 0, sizeof(*state));

    state->entries = domain_platform_alloc(max_entity_types * sizeof(*state->entries));
    if (state->entries == NULL) {
        domain_platform_free(state);
        return domain_fail(SYS_CODE_NO_MEM);
    }
    memset(state->entries, 0, max_entity_types * sizeof(*state->entries));

    state->lock = domain_platform_lock_create();
    if (state->lock == NULL) {
        domain_platform_free(state->entries);
        domain_platform_free(state);
        return domain_fail(SYS_CODE_NO_MEM);
    }
    const sys_error_t journal_err = domain_journal_init(&state->journal, journal_capacity);
    if (sys_failed(journal_err)) {
        domain_platform_lock_destroy(state->lock);
        domain_platform_free(state->entries);
        domain_platform_free(state);
        return journal_err;
    }

    const sys_error_t payload_err = domain_payload_init(&state->payload, payload_capacity,
                                                        payload_max_size);
    if (sys_failed(payload_err)) {
        (void)domain_journal_deinit(&state->journal);
        domain_platform_lock_destroy(state->lock);
        domain_platform_free(state->entries);
        domain_platform_free(state);
        return payload_err;
    }

    const sys_error_t dispatch_err = domain_dispatch_init(state);
    if (sys_failed(dispatch_err)) {
        domain_payload_deinit(&state->payload);
        domain_dispatch_deinit(state);
        (void)domain_journal_deinit(&state->journal);
        domain_platform_lock_destroy(state->lock);
        domain_platform_free(state->entries);
        domain_platform_free(state);
        return dispatch_err;
    }

    state->capacity = max_entity_types;

    domain->_state = state;
    return SYS_OK;
}

sys_error_t domain_deinit(domain_t *domain)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL) {
        return domain_fail(SYS_CODE_INVALID_STATE);
    }

    domain_commands_deinit(state);
    domain_dispatch_deinit(state);
    domain_payload_deinit(&state->payload);
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
    return SYS_OK;
}

sys_error_t domain_register_entity(domain_t *domain, const domain_entity_desc_t *desc)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || desc == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    const sys_error_t checked = desc_validate(desc);
    if (sys_failed(checked)) {
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
    sys_error_t result = SYS_OK;
    if (domain_entry_find(state, desc->type) != NULL) {
        result = domain_fail(SYS_CODE_INVALID_STATE);
    } else if (state->used == state->capacity) {
        result = domain_fail(SYS_CODE_NO_SPACE);
    }
    domain_platform_lock_release(state->lock);
    if (sys_failed(result)) {
        return result;
    }

    void *scratch_key = domain_platform_alloc(desc->key_size);
    if (scratch_key == NULL) {
        return domain_fail(SYS_CODE_NO_MEM);
    }

    mstore_table_t table = {0};
    const sys_error_t err = mstore_table_init(&table, &schema);
    if (sys_failed(err)) {
        domain_platform_free(scratch_key);
        return err; /* ошибка mstore идёт дальше без перекодирования */
    }

    domain_platform_lock_acquire(state->lock);
    if (domain_entry_find(state, desc->type) != NULL || state->used == state->capacity) {
        domain_platform_lock_release(state->lock);
        (void)mstore_table_deinit(&table);
        domain_platform_free(scratch_key);
        return domain_fail(SYS_CODE_INVALID_STATE);
    }

    domain_entity_entry_t *entry = &state->entries[state->used];
    entry->desc = *desc;
    entry->table = table;
    entry->scratch_key = scratch_key;
    entry->used = true;
    state->used++;
    domain_platform_lock_release(state->lock);
    return SYS_OK;
}
