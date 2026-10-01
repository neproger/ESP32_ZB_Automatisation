#include "domain/domain.h"

#include <assert.h>
#include <string.h>

#include "domain_internal.h"
#include "domain_platform.h"

/*
 * Сборка записи Journal. Здесь и только здесь определяется форма факта: операции
 * передают смысл (kind / op / результат), а не поля (JOURNAL.md §2).
 *
 * Инварианты, а не проверки: key_size задан регистрацией типа сущности и не может
 * быть шире DOMAIN_EVENT_KEY_MAX; source обязан быть меньше DOMAIN_SOURCE_ID_MAX,
 * потому что кодируется битом в маске подписки. В release assert стоит ноль.
 */
void domain_fact_write(domain_state_t *state, const domain_fact_meta_t *meta,
                       domain_entity_t type, const void *key, uint8_t key_size, uint8_t kind,
                       uint8_t op, sys_error_t error, uint64_t payload_ref)
{
    assert(state != NULL);
    assert(key_size <= DOMAIN_EVENT_KEY_MAX);
    assert(meta == NULL || meta->source < DOMAIN_SOURCE_ID_MAX);

    domain_event_t fact = {0};
    fact.ts = domain_platform_now_ms();
    fact.kind = kind;
    fact.op = op;
    fact.key_size = key_size;
    fact.entity = type;
    fact.error = error;
    /* Ссылку на payload операция передаёт явно (put знает seq); из meta берём только
     * если самой операции ссылки неоткуда взять. */
    fact.payload_ref = (payload_ref != 0 || meta == NULL) ? payload_ref : meta->payload_ref;
    if (meta != NULL) {
        fact.source = meta->source;
        fact.value = meta->value;
    } else {
        fact.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    }
    if (key != NULL && key_size > 0) {
        memcpy(fact.key, key, key_size);
    }

    domain_event_id_t id = 0;
    (void)domain_journal_append(&state->journal, &fact, &id);
    domain_dispatch_signal(state);
}

sys_error_t domain_fact_target_resolve(domain_state_t *state, const domain_fact_target_t *target,
                                       domain_entity_t *out_entity, const void **out_key,
                                       uint8_t *out_key_size)
{
    if (state == NULL || out_entity == NULL || out_key == NULL || out_key_size == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    *out_entity = 0;
    *out_key = NULL;
    *out_key_size = 0;

    if (target == NULL) {
        return SYS_OK; /* факт без адресата: команда или событие «ни о ком» */
    }
    if (target->key == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    const domain_entity_entry_t *entry = domain_entry_find(state, target->entity);
    if (entry == NULL) {
        return domain_fail(SYS_CODE_NOT_FOUND);
    }

    *out_entity = target->entity;
    *out_key = target->key;
    *out_key_size = (uint8_t)entry->desc.key_size;
    return SYS_OK;
}
