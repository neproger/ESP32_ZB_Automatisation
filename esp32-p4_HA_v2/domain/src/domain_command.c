#include "domain/domain.h"

#include "domain_internal.h"
#include "domain_platform.h"

/*
 * Команда — transient intent, а не состояние (docs/domain/COMMANDS.md).
 *
 * Два независимых аспекта:
 *   delivery — напрямую executor'у, синхронно в контексте вызывающего;
 *   audit    — тонкая запись COMMAND_SENT в Journal, и только после успешной передачи.
 *
 * Domain не знает предметной области: только discriminator и callback.
 */

sys_error_t domain_register_command(domain_t *domain, domain_command_t type,
                                    domain_command_fn executor, void *ctx)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || executor == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    struct domain_command_entry *entry = domain_platform_alloc(sizeof(*entry));
    if (entry == NULL) {
        return domain_fail(SYS_CODE_NO_MEM);
    }
    entry->type = type;
    entry->executor = executor;
    entry->ctx = ctx;

    domain_platform_lock_acquire(state->lock);
    sys_error_t result = SYS_OK;
    if (domain_command_find(state, type) != NULL) {
        result = domain_fail(SYS_CODE_INVALID_STATE);
    } else {
        entry->next = state->commands;
        state->commands = entry;
    }
    domain_platform_lock_release(state->lock);

    if (sys_failed(result)) {
        domain_platform_free(entry);
    }
    return result;
}

sys_error_t domain_post(domain_t *domain, domain_command_t type, const void *args,
                        size_t args_size, const domain_fact_target_t *target,
                        const domain_fact_meta_t *meta)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    /* Адресат проверяем до executor'а: при неизвестном типе факта не будет вовсе. */
    domain_platform_lock_acquire(state->lock);
    domain_entity_t target_entity = 0;
    const void *target_key = NULL;
    uint8_t target_key_size = 0;
    const sys_error_t resolved =
        domain_fact_target_resolve(state, target, &target_entity, &target_key, &target_key_size);
    domain_platform_lock_release(state->lock);
    if (sys_failed(resolved)) {
        return resolved;
    }

    /* Executor ищем под lock'ом, а вызываем уже без него: executor — внешний код и
     * вправе сам обращаться к Domain (DOMAIN_API.md §9). */
    domain_platform_lock_acquire(state->lock);
    const struct domain_command_entry *entry = domain_command_find(state, type);
    domain_command_fn executor = (entry == NULL) ? NULL : entry->executor;
    void *ctx = (entry == NULL) ? NULL : entry->ctx;
    domain_platform_lock_release(state->lock);

    if (executor == NULL) {
        return domain_fail(SYS_CODE_NOT_FOUND);
    }

    const sys_error_t delivered = executor(type, args, args_size, ctx);
    if (sys_failed(delivered)) {
        return delivered; /* отклонил — факта нет */
    }

    domain_platform_lock_acquire(state->lock);
    domain_fact_write(state, meta, target_entity, target_key, target_key_size,
                      (uint8_t)DOMAIN_FACT_COMMAND_SENT, (uint8_t)DOMAIN_OP_COMMAND, SYS_OK, 0);
    domain_platform_lock_release(state->lock);
    return SYS_OK;
}

struct domain_command_entry *domain_command_find(domain_state_t *state, domain_command_t type)
{
    if (state == NULL) {
        return NULL;
    }
    for (struct domain_command_entry *entry = state->commands; entry != NULL;
         entry = entry->next) {
        if (entry->type == type) {
            return entry;
        }
    }
    return NULL;
}

void domain_commands_deinit(domain_state_t *state)
{
    if (state == NULL) {
        return;
    }
    struct domain_command_entry *entry = state->commands;
    while (entry != NULL) {
        struct domain_command_entry *next = entry->next;
        domain_platform_free(entry);
        entry = next;
    }
    state->commands = NULL;
}
