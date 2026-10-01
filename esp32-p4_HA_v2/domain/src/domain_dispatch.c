#include "domain/domain.h"

#include "domain_internal.h"
#include "domain_platform.h"

/*
 * Dispatcher — доставка фактов подписчикам. Здесь только маршрутизация:
 * фильтр, push в inbox, пробуждение. Логику подписчика Dispatcher не выполняет и не
 * ждёт (DISPATCHER.md §1-2).
 *
 * Синхронизация:
 *   - список подписок и курсор Journal — под dispatch_lock;
 *   - Journal дополнительно защищён своим lock'ом внутри mstore;
 *   - общий Domain-lock здесь **не** удерживается: медленный подписчик не имеет права
 *     блокировать Entity Store. Цена — доставка идёт под dispatch_lock, поэтому
 *     затянувшийся try_push задерживает доставку остальным подписчикам, но не операции
 *     над сущностями.
 */

struct domain_subscription {
    domain_subscription_desc_t desc;
    struct domain_subscription *next;
};

bool domain_outcome_is_runtime_error(domain_err_t err)
{
    switch (err) {
    case DOMAIN_IO:
    case DOMAIN_CORRUPT:
    case DOMAIN_NO_SPACE:
    case DOMAIN_NO_MEM:
    case DOMAIN_OVERFLOW:
    case DOMAIN_BUSY:
        return true;
    default:
        return false;
    }
}

static bool matches(const domain_subscription_desc_t *desc, const domain_event_t *event)
{
    if (desc->kind_mask != 0 && (desc->kind_mask & (1u << event->kind)) == 0) {
        return false;
    }
    if (desc->source_mask != 0 && (desc->source_mask & (1u << event->source)) == 0) {
        return false;
    }
    if (desc->entity != 0 && desc->entity != event->entity) {
        return false;
    }
    return true;
}

domain_err_t domain_dispatch_init(domain_state_t *state)
{
    state->subscriptions = NULL;
    state->gap_count = 0;

    state->dispatch_lock = domain_platform_lock_create();
    if (state->dispatch_lock == NULL) {
        return DOMAIN_NO_MEM;
    }

    state->signal = domain_platform_signal_create();
    if (state->signal == NULL) {
        domain_platform_lock_destroy(state->dispatch_lock);
        state->dispatch_lock = NULL;
        return DOMAIN_NO_MEM;
    }

    /* Курсор встаёт на следующий ожидаемый факт: история при старте не реплеится. */
    domain_event_id_t newest = 0;
    state->cursor = (domain_journal_newest(&state->journal, &newest) == DOMAIN_OK) ? newest + 1
                                                                                  : 1;
    return DOMAIN_OK;
}

void domain_dispatch_deinit(domain_state_t *state)
{
    struct domain_subscription *sub = state->subscriptions;
    while (sub != NULL) {
        struct domain_subscription *next = sub->next;
        domain_platform_free(sub);
        sub = next;
    }
    state->subscriptions = NULL;

    if (state->signal != NULL) {
        domain_platform_signal_destroy(state->signal);
        state->signal = NULL;
    }
    if (state->dispatch_lock != NULL) {
        domain_platform_lock_destroy(state->dispatch_lock);
        state->dispatch_lock = NULL;
    }
}

void domain_dispatch_signal(domain_state_t *state)
{
    if (state->signal != NULL) {
        domain_platform_signal_raise(state->signal);
    }
}

domain_err_t domain_subscribe(domain_t *domain, const domain_subscription_desc_t *desc,
                              domain_subscription_t **out_sub)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || desc == NULL || out_sub == NULL || desc->try_push == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    domain_subscription_t *sub = domain_platform_alloc(sizeof(*sub));
    if (sub == NULL) {
        return DOMAIN_NO_MEM;
    }
    sub->desc = *desc;
    sub->next = NULL;

    domain_platform_lock_acquire(state->dispatch_lock);
    sub->next = state->subscriptions;
    state->subscriptions = sub;
    domain_platform_lock_release(state->dispatch_lock);

    *out_sub = sub;
    return DOMAIN_OK;
}

domain_err_t domain_unsubscribe(domain_t *domain, domain_subscription_t *sub)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || sub == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    domain_platform_lock_acquire(state->dispatch_lock);
    domain_subscription_t **link = &state->subscriptions;
    while (*link != NULL && *link != sub) {
        link = &(*link)->next;
    }
    const bool found = (*link == sub);
    if (found) {
        *link = sub->next;
    }
    domain_platform_lock_release(state->dispatch_lock);

    if (!found) {
        /* Чужой или уже отписанный handle: освобождать его Domain не имеет права. */
        return DOMAIN_NOT_FOUND;
    }

    domain_platform_free(sub);
    return DOMAIN_OK;
}

domain_err_t domain_dispatch_wait(domain_t *domain, uint32_t timeout_ms, bool *out_signalled)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || out_signalled == NULL) {
        return DOMAIN_INVALID_ARG;
    }
    if (state->signal == NULL) {
        return DOMAIN_INVALID_STATE;
    }

    *out_signalled = domain_platform_signal_wait(state->signal, timeout_ms);
    return DOMAIN_OK;
}

domain_err_t domain_dispatch_once(domain_t *domain, size_t *out_delivered)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    size_t delivered = 0;

    domain_platform_lock_acquire(state->dispatch_lock);

    domain_event_id_t newest = 0;
    if (domain_journal_newest(&state->journal, &newest) != DOMAIN_OK) {
        domain_platform_lock_release(state->dispatch_lock);
        if (out_delivered != NULL) {
            *out_delivered = 0;
        }
        return DOMAIN_OK;
    }

    domain_event_id_t oldest = 0;
    if (domain_journal_oldest(&state->journal, &oldest) == DOMAIN_OK && state->cursor < oldest) {
        /* Факты вытеснены раньше, чем их прочитали: фиксируем и догоняем с oldest. */
        state->gap_count++;
        state->cursor = oldest;
    }

    const domain_event_id_t last = newest;
    while (state->cursor <= last) {
        domain_event_t event = {0};
        const domain_err_t err = domain_journal_get(&state->journal, state->cursor, &event);
        state->cursor++;
        if (err != DOMAIN_OK) {
            continue; /* вытеснено во время разбора */
        }

        for (domain_subscription_t *sub = state->subscriptions; sub != NULL; sub = sub->next) {
            if (!matches(&sub->desc, &event)) {
                continue;
            }
            if (sub->desc.try_push(&event, sub->desc.ctx)) {
                delivered++;
                if (sub->desc.wake != NULL) {
                    sub->desc.wake(sub->desc.ctx);
                }
            }
            /* false — inbox подписчика полон: его локальная потеря, доставка продолжается. */
        }
    }

    domain_platform_lock_release(state->dispatch_lock);

    if (out_delivered != NULL) {
        *out_delivered = delivered;
    }
    return DOMAIN_OK;
}
