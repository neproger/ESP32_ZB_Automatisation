#pragma once

#include <stddef.h>

#include "domain/domain_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Domain — инфраструктурное ядро фактов: хранит, журналирует, доставляет.
 * Семантика принадлежит сервисам (docs/ARCHITECTURE.md §3).
 */
typedef struct {
    void *_state;
} domain_t;

/*
 * Поднимает Domain и его registry типов. max_entity_types — максимальное число
 * зарегистрированных типов сущностей; ёмкость самих таблиц задаётся в descriptor.
 */
domain_err_t domain_init(domain_t *domain, size_t max_entity_types, size_t journal_capacity);
domain_err_t domain_deinit(domain_t *domain);

/*
 * Регистрирует тип сущности и поднимает под него таблицу хранилища.
 * Вызывается из bootstrap приложения (docs/RECORD_MODEL.md §1.1), а не из сервиса.
 */
domain_err_t domain_register_entity(domain_t *domain, const domain_entity_desc_t *desc);

/*
 * Обход записей типа. Указатели key/record действительны только во время вызова и
 * не должны сохраняться; мутирующий API Domain внутри колбэка вызывать нельзя
 * (docs/domain/DOMAIN_API.md §8-9). Возврат false прерывает обход.
 */
typedef bool (*domain_entity_iter_cb_t)(const void *key, const void *record, void *ctx);

/*
 * Компактное описание факта от того, кто меняет состояние: Domain не «догадывается»,
 * что писать в Journal, а получает это от вызывающего (ENTITY_STORE.md §4).
 * ts / event_id / entity / key / op добавляет сам Domain.
 */
typedef struct {
    uint8_t source;
    domain_value_t value;
    uint64_t payload_ref;
} domain_fact_meta_t;

/*
 * Запись сущности. Размеры key и record заданы descriptor'ом типа.
 * out_changed обязателен: «записали» и «ничего не изменилось» — разные исходы.
 *
 * Факт в Journal пишется только при changed = true (ENTITY_STORE.md §8).
 * Если append вернул ошибку, состояние уже записано: в штатном пути append не
 * отказывает, ошибка означает нарушение внутреннего контракта, а не бизнес-исход.
 */
domain_err_t domain_entity_put(domain_t *domain, domain_entity_t type,
                               const void *key, const void *record,
                               const domain_fact_meta_t *meta, bool *out_changed);
domain_err_t domain_entity_get(domain_t *domain, domain_entity_t type,
                               const void *key, void *out_record);
domain_err_t domain_entity_remove(domain_t *domain, domain_entity_t type, const void *key,
                                  const domain_fact_meta_t *meta);
domain_err_t domain_entity_iter(domain_t *domain, domain_entity_t type,
                                domain_entity_iter_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
