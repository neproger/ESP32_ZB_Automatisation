#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/domain_event.h"
#include "domain/domain_types.h"
#include "mstore/mstore_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Journal — внутренняя подсистема Domain: поток фактов поверх Ring Store.
 * Он не классифицирует, не фильтрует и не проверяет факты: это делает операция
 * Domain (docs/domain/JOURNAL.md §2.1-2.2).
 *
 * Сервисы журнал не читают: единственный потребитель — Dispatcher
 * (docs/domain/JOURNAL.md §5, DISPATCHER.md §5).
 */

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

/*
 * Классификация исхода операции живёт здесь не потому, что её дело — Journal, а потому
 * что это единственное место, где перечислены runtime-ошибки Domain. Решение
 * «журналировать или нет» принимает операция, а не хранилище фактов.
 */
bool domain_journal_is_runtime_error(domain_err_t err);

#ifdef __cplusplus
}
#endif
