#include "domain/domain.h"

#include "domain_internal.h"
#include "domain_platform.h"

/*
 * Dispatcher — доставка фактов подписчикам. Здесь только маршрутизация:
 * фильтр, push в inbox, пробуждение. Логику подписчика Dispatcher не выполняет и не
 * ждёт (DISPATCHER.md §1-2).
 */

struct domain_subscription {
    domain_subscription_desc_t desc;
    struct domain_subscription *next;
};

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

    state->signal = domain_platform_signal_create();
    if (state->signal == NULL) {
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
    state->subscriptions = NULL;
    if (state->signal != NULL) {
        domain_platform_signal_destroy(state->signal);
        state->signal = NULL;
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

    domain_platform_lock_acquire(state->lock);
    sub->next = state->subscriptions;
    state->subscriptions = sub;
    domain_platform_lock_release(state->lock);

    *out_sub = sub;
    return DOMAIN_OK;
}

domain_err_t domain_unsubscribe(domain_t *domain, domain_subscription_t *sub)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || sub == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    domain_platform_lock_acquire(state->lock);
    domain_subscription_t **link = &state->subscriptions;
    while (*link != NULL) {
        if (*link == sub) {
            *link = sub->next;
            break;
        }
        link = &(*link)->next;
    }
    domain_platform_lock_release(state->lock);

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

static void deliver(domain_state_t *state, const domain_event_t *event, size_t *out_delivered)
{
    for (domain_subscription_t *sub = state->subscriptions; sub != NULL; sub = sub->next) {
        if (!matches(&sub->desc, event)) {
            continue;
        }
        if (sub->desc.try_push(event, sub->desc.ctx)) {
            (*out_delivered)++;
            if (sub->desc.wake != NULL) {
                sub->desc.wake(sub->desc.ctx);
            }
        }
        /* false — inbox подписчика полон: его локальная потеря, доставка продолжается. */
    }
}

domain_err_t domain_dispatch_once(domain_t *domain, size_t *out_delivered)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    domain_platform_lock_acquire(state->lock);
    if (out_delivered != NULL) {
        *out_delivered = 0;
    }

    domain_event_id_t newest = 0;
    if (domain_journal_newest(&state->journal, &newest) != DOMAIN_OK) {
        domain_platform_lock_release(state->lock);
        return DOMAIN_OK;
    }

    domain_event_id_t oldest = 0;
    if (domain_journal_oldest(&state->journal, &oldest) == DOMAIN_OK && state->cursor < oldest) {
        /* Факты вытеснены раньше, чем их прочитали: фиксируем и догоняем с oldest. */
        state->gap_count++;
        state->cursor = oldest;
    }

    const domain_event_id_t last = newest;
    for (domain_event_id_t id = state->cursor; id <= last; ++id) {
        domain_event_t event = {0};
        const domain_err_t err = domain_journal_get(&state->journal, id, &event);
        if (err != DOMAIN_OK) {
            continue; /* вытеснено во время разбора — пропускаем, курсор всё равно идёт */
        }
        if (out_delivered != NULL) {
            deliver(state, &event, out_delivered);
        }
    }

    state->cursor = last + 1;
    domain_platform_lock_release(state->lock);
    return DOMAIN_OK;
}
