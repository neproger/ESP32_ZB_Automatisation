#include "domain/domain.h"

#include <string.h>

#include "domain_internal.h"
#include "domain_platform.h"
#include "mstore/mstore_ring.h"

/*
 * Transient payload — best-effort окно для данных, которым тесно в compact value
 * (docs/domain/TRANSIENT_PAYLOAD.md).
 *
 * Никакого ownership / refcount / release / TTL: payload живёт, пока его не вытеснит
 * ring. Ссылка — seq записи ring'а, для сервиса полностью opaque.
 *
 * Запись фиксированного размера: сначала размер payload, затем его байты. Размер
 * Storage'ом не интерпретируется; ring вытесняет запись целиком.
 */

#define PAYLOAD_SIZE_FIELD sizeof(uint32_t)

static size_t payload_record_size(const domain_payload_t *payload)
{
    return PAYLOAD_SIZE_FIELD + payload->max_size;
}

sys_error_t domain_payload_init(domain_payload_t *payload, size_t capacity, size_t max_size)
{
    if (payload == NULL || capacity == 0 || max_size == 0) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    memset(payload, 0, sizeof(*payload));
    payload->max_size = max_size;
    payload->scratch = domain_platform_alloc(payload_record_size(payload));
    if (payload->scratch == NULL) {
        return domain_fail(SYS_CODE_NO_MEM);
    }

    mstore_ring_config_t config = {0};
    config.capacity = capacity;
    config.record_size = payload_record_size(payload);
    const sys_error_t err = mstore_ring_init(&payload->ring, &config);
    if (sys_failed(err)) {
        domain_platform_free(payload->scratch);
        payload->scratch = NULL;
        return err; /* ошибка mstore идёт дальше без перекодирования */
    }
    return SYS_OK;
}

void domain_payload_deinit(domain_payload_t *payload)
{
    if (payload == NULL || payload->scratch == NULL) {
        return;
    }
    (void)mstore_ring_deinit(&payload->ring);
    domain_platform_free(payload->scratch);
    memset(payload, 0, sizeof(*payload));
}

/*
 * put — это «опубликовать событие с данными»: payload копируется в ring, а в Journal
 * уходит факт EVENT со ссылкой. Факт осмыслен и без payload (JOURNAL.md §3), поэтому
 * вытеснение payload — потеря деталей, а не потеря события.
 */
sys_error_t domain_payload_put(domain_t *domain, const domain_fact_target_t *target,
                               const domain_fact_meta_t *meta, const void *payload, size_t size,
                               domain_payload_ref_t *out_ref)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || payload == NULL || out_ref == NULL || size == 0) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }
    if (size > state->payload.max_size) {
        return domain_fail(SYS_CODE_INVALID_SIZE);
    }

    domain_platform_lock_acquire(state->lock);

    domain_entity_t target_entity = 0;
    const void *target_key = NULL;
    uint8_t target_key_size = 0;
    const sys_error_t resolved =
        domain_fact_target_resolve(state, target, &target_entity, &target_key, &target_key_size);
    if (sys_failed(resolved)) {
        domain_platform_lock_release(state->lock);
        return resolved;
    }

    uint8_t *scratch = (uint8_t *)state->payload.scratch;
    const uint32_t stored_size = (uint32_t)size;
    memcpy(scratch, &stored_size, PAYLOAD_SIZE_FIELD);
    memcpy(scratch + PAYLOAD_SIZE_FIELD, payload, size);

    uint64_t seq = 0;
    sys_error_t err = mstore_ring_append(&state->payload.ring, scratch, &seq);
    if (sys_failed(err)) {
        domain_fact_write(state, meta, target_entity, target_key, target_key_size,
                          (uint8_t)DOMAIN_FACT_ERROR, (uint8_t)DOMAIN_OP_PAYLOAD_PUT, err, 0);
        domain_platform_lock_release(state->lock);
        return err;
    }

    domain_fact_write(state, meta, target_entity, target_key, target_key_size,
                      (uint8_t)DOMAIN_FACT_EVENT, (uint8_t)DOMAIN_OP_PAYLOAD_PUT, SYS_OK, seq);
    domain_platform_lock_release(state->lock);

    *out_ref = seq;
    return SYS_OK;
}

sys_error_t domain_payload_get(domain_t *domain, domain_payload_ref_t ref, void *out,
                               size_t out_size)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || out == NULL || out_size == 0) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    domain_platform_lock_acquire(state->lock);
    const sys_error_t err = mstore_ring_get_by_seq(&state->payload.ring, ref,
                                                   state->payload.scratch);
    if (sys_failed(err)) {
        domain_platform_lock_release(state->lock);
        return err; /* STALE — вытеснен, NOT_FOUND — ссылка неизвестна */
    }

    /* Копируем под lock: scratch — общий буфер на весь Domain. */
    const uint8_t *scratch = (const uint8_t *)state->payload.scratch;
    uint32_t stored_size = 0;
    memcpy(&stored_size, scratch, PAYLOAD_SIZE_FIELD);
    if (stored_size != (uint32_t)out_size) {
        domain_platform_lock_release(state->lock);
        return domain_fail(SYS_CODE_INVALID_SIZE);
    }
    memcpy(out, scratch + PAYLOAD_SIZE_FIELD, stored_size);
    domain_platform_lock_release(state->lock);
    return SYS_OK;
}
