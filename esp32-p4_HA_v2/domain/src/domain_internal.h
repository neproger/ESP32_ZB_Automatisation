#pragma once

#include <stdbool.h>

#include "domain/domain.h"
#include "domain/domain_types.h"
#include "mstore/mstore_table.h"
#include "mstore/mstore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool used;
    domain_entity_desc_t desc;
    mstore_table_t table;

    /*
     * scratch под key: mstore отдаёт meta + key + payload одним snapshot'ом,
     * а чтению сущности ключ не нужен. Буфер на тип, а не на вызов: аллокаций
     * в read-path быть не должно.
     */
    void *scratch_key;
} domain_entity_entry_t;

typedef struct {
    domain_entity_entry_t *entries;
    size_t used;
    size_t capacity;

    /*
     * Сериализует весь mutation path и read path: порядок «запись + факт»,
     * доступ к scratch_key и обход. Lock не рекурсивный — колбэк iter не имеет
     * права вызывать API Domain (docs/domain/DOMAIN_API.md §9).
     */
    void *lock;
} domain_state_t;

domain_state_t *domain_state(const domain_t *domain);
domain_entity_entry_t *domain_entry_find(domain_state_t *state, domain_entity_t type);

domain_err_t domain_err_from_mstore(mstore_err_t err);

#ifdef __cplusplus
}
#endif
