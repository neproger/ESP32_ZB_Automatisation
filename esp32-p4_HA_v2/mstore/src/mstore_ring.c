#include <string.h>

#include "mstore/mstore_ring.h"
#include "mstore_platform.h"

typedef struct {
    size_t capacity;
    size_t record_size;
    uint8_t *records;
    uint64_t next_seq; /* следующий выдаваемый seq; начинается с 1 */
    size_t count;
    void *lock;
} mstore_ring_state_t;

static uint8_t *mstore_ring_record(const mstore_ring_state_t *st, uint64_t seq) {
    size_t slot = (size_t)((seq - 1) % st->capacity);
    return st->records + slot * st->record_size;
}

static uint64_t mstore_ring_oldest(const mstore_ring_state_t *st) {
    return st->next_seq - st->count;
}

static void mstore_ring_destroy(mstore_ring_state_t *st) {
    if (st == NULL) {
        return;
    }
    if (st->lock != NULL) {
        mstore_platform_lock_destroy(st->lock);
    }
    mstore_platform_free(st->records);
    mstore_platform_free(st);
}

mstore_err_t mstore_ring_init(mstore_ring_t *ring, const mstore_ring_config_t *config) {
    if (ring == NULL || config == NULL) {
        return MSTORE_INVALID_ARG;
    }
    if (ring->_state != NULL) {
        return MSTORE_INVALID_STATE;
    }
    if (config->capacity == 0 || config->record_size == 0) {
        return MSTORE_INVALID_SIZE;
    }
    if (config->capacity > SIZE_MAX / config->record_size) {
        return MSTORE_INVALID_SIZE;
    }

    mstore_ring_state_t *st = mstore_platform_alloc(sizeof(*st));
    if (st == NULL) {
        return MSTORE_NO_MEM;
    }
    memset(st, 0, sizeof(*st));

    st->capacity = config->capacity;
    st->record_size = config->record_size;
    st->next_seq = 1;
    st->count = 0;

    st->records = mstore_platform_alloc(st->capacity * st->record_size);
    st->lock = mstore_platform_lock_create();
    if (st->records == NULL || st->lock == NULL) {
        mstore_ring_destroy(st);
        return MSTORE_NO_MEM;
    }

    ring->_state = st;
    return MSTORE_OK;
}

mstore_err_t mstore_ring_deinit(mstore_ring_t *ring) {
    if (ring == NULL) {
        return MSTORE_INVALID_ARG;
    }
    if (ring->_state == NULL) {
        return MSTORE_INVALID_STATE;
    }
    mstore_ring_destroy((mstore_ring_state_t *)ring->_state);
    ring->_state = NULL;
    return MSTORE_OK;
}

mstore_err_t mstore_ring_append(mstore_ring_t *ring, const void *record, uint64_t *out_seq) {
    if (ring == NULL || record == NULL || out_seq == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_ring_state_t *st = ring->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);
    uint64_t seq = st->next_seq;
    memcpy(mstore_ring_record(st, seq), record, st->record_size);
    st->next_seq++;
    if (st->count < st->capacity) {
        st->count++;
    }
    *out_seq = seq;
    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}

mstore_err_t mstore_ring_get_by_seq(const mstore_ring_t *ring, uint64_t seq, void *out_record) {
    if (ring == NULL || out_record == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_ring_state_t *st = ring->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_err_t err;
    if (seq == 0 || seq >= st->next_seq) {
        err = MSTORE_NOT_FOUND; /* seq ещё не существовал */
    } else if (seq < mstore_ring_oldest(st)) {
        err = MSTORE_STALE; /* вытеснен из окна */
    } else {
        memcpy(out_record, mstore_ring_record(st, seq), st->record_size);
        err = MSTORE_OK;
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

mstore_err_t mstore_ring_oldest_seq(const mstore_ring_t *ring, uint64_t *out_seq) {
    if (ring == NULL || out_seq == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_ring_state_t *st = ring->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_err_t err = st->count == 0 ? MSTORE_NOT_FOUND : MSTORE_OK;
    if (err == MSTORE_OK) {
        *out_seq = mstore_ring_oldest(st);
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

mstore_err_t mstore_ring_newest_seq(const mstore_ring_t *ring, uint64_t *out_seq) {
    if (ring == NULL || out_seq == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_ring_state_t *st = ring->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_err_t err = st->count == 0 ? MSTORE_NOT_FOUND : MSTORE_OK;
    if (err == MSTORE_OK) {
        *out_seq = st->next_seq - 1;
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

mstore_err_t mstore_ring_contains(const mstore_ring_t *ring, uint64_t seq, bool *out_contains) {
    if (ring == NULL || out_contains == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_ring_state_t *st = ring->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);
    *out_contains = st->count > 0 && seq >= mstore_ring_oldest(st) && seq < st->next_seq;
    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}

mstore_err_t mstore_ring_count(const mstore_ring_t *ring, size_t *out_count) {
    if (ring == NULL || out_count == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_ring_state_t *st = ring->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);
    *out_count = st->count;
    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}
