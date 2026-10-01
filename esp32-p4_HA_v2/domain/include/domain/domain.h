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
domain_err_t domain_init(domain_t *domain, size_t max_entity_types);
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
 * Запись сущности. Размеры key и record заданы descriptor'ом типа.
 * out_changed обязателен: «записали» и «ничего не изменилось» — разные исходы.
 */
domain_err_t domain_entity_put(domain_t *domain, domain_entity_t type,
                               const void *key, const void *record, bool *out_changed);
domain_err_t domain_entity_get(domain_t *domain, domain_entity_t type,
                               const void *key, void *out_record);
domain_err_t domain_entity_remove(domain_t *domain, domain_entity_t type, const void *key);
domain_err_t domain_entity_iter(domain_t *domain, domain_entity_t type,
                                domain_entity_iter_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
