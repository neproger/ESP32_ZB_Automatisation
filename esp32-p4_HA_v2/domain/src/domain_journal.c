#include "domain_journal.h"

#include <string.h>

#include "domain_internal.h"

/*
 * Journal — тонкий слой над Ring Store: identity факта (event_id) это seq ring'а,
 * поэтому отдельной таблицы соответствий нет (MSTORE.md §5).
 *
 * Семантика чтения:
 *   id в окне        → OK
 *   id меньше oldest → STALE (вытеснен; это и есть признак Journal-gap)
 *   id больше newest → NOT_FOUND (ещё не случился)
 */
sys_error_t domain_journal_init(domain_journal_t *journal, size_t capacity)
{
    if (journal == NULL || capacity == 0) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    mstore_ring_config_t config = {0};
    config.capacity = capacity;
    config.record_size = sizeof(domain_event_t);
    return mstore_ring_init(&journal->ring, &config);
}

sys_error_t domain_journal_deinit(domain_journal_t *journal)
{
    if (journal == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    return mstore_ring_deinit(&journal->ring);
}

sys_error_t domain_journal_append(domain_journal_t *journal, domain_event_t *event,
                                   domain_event_id_t *out_id)
{
    if (journal == NULL || event == NULL || out_id == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    uint64_t seq = 0;
    const sys_error_t err = mstore_ring_append(&journal->ring, event, &seq);
    if (sys_failed(err)) {
        return err;
    }

    event->event_id = seq;
    *out_id = seq;
    return SYS_OK;
}

sys_error_t domain_journal_get(const domain_journal_t *journal, domain_event_id_t id,
                                domain_event_t *out)
{
    if (journal == NULL || out == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    const sys_error_t result = mstore_ring_get_by_seq(&journal->ring, id, out);
    if (sys_failed(result)) {
        return result;
    }
    out->event_id = id;
    return SYS_OK;
}

sys_error_t domain_journal_oldest(const domain_journal_t *journal, domain_event_id_t *out)
{
    if (journal == NULL || out == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    return mstore_ring_oldest_seq(&journal->ring, out);
}

sys_error_t domain_journal_newest(const domain_journal_t *journal, domain_event_id_t *out)
{
    if (journal == NULL || out == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    return mstore_ring_newest_seq(&journal->ring, out);
}

sys_error_t domain_journal_count(const domain_journal_t *journal, size_t *out)
{
    if (journal == NULL || out == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    return mstore_ring_count(&journal->ring, out);
}

sys_error_t domain_journal_contains(const domain_journal_t *journal, domain_event_id_t id,
                                     bool *out)
{
    if (journal == NULL || out == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    return mstore_ring_contains(&journal->ring, id, out);
}
