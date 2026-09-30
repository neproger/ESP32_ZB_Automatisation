#include "nor_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mstore_nor_sim {
    size_t size;
    size_t erase_size;
    uint8_t *bytes;
    size_t program_count;
    size_t erase_count;
    bool fail_program_armed;
    size_t fail_program_after;
    bool fail_erase_armed;
};

static bool range_ok(const mstore_nor_sim_t *sim, size_t offset, size_t len) {
    return offset <= sim->size && len <= sim->size - offset;
}

mstore_nor_sim_t *mstore_nor_sim_create(size_t size, size_t erase_size) {
    if (size == 0 || erase_size == 0) {
        return NULL;
    }
    mstore_nor_sim_t *sim = malloc(sizeof(*sim));
    if (sim == NULL) {
        return NULL;
    }
    sim->bytes = malloc(size);
    if (sim->bytes == NULL) {
        free(sim);
        return NULL;
    }
    sim->size = size;
    sim->erase_size = erase_size;
    sim->program_count = 0;
    sim->erase_count = 0;
    sim->fail_program_armed = false;
    sim->fail_program_after = 0;
    sim->fail_erase_armed = false;
    memset(sim->bytes, 0xFF, size);
    return sim;
}

void mstore_nor_sim_destroy(mstore_nor_sim_t *sim) {
    if (sim == NULL) {
        return;
    }
    free(sim->bytes);
    free(sim);
}

size_t mstore_nor_sim_size(const mstore_nor_sim_t *sim) {
    return sim->size;
}

size_t mstore_nor_sim_erase_size(const mstore_nor_sim_t *sim) {
    return sim->erase_size;
}

bool mstore_nor_sim_read(const mstore_nor_sim_t *sim, size_t offset, void *dst, size_t len) {
    if (sim == NULL || dst == NULL || !range_ok(sim, offset, len)) {
        return false;
    }
    memcpy(dst, sim->bytes + offset, len);
    return true;
}

bool mstore_nor_sim_program(mstore_nor_sim_t *sim, size_t offset, const void *src, size_t len) {
    if (sim == NULL || src == NULL || !range_ok(sim, offset, len)) {
        return false;
    }

    const uint8_t *in = src;
    for (size_t i = 0; i < len; i++) {
        if (((uint8_t)~sim->bytes[offset + i] & in[i]) != 0) {
            return false; /* попытка 0 -> 1 без erase */
        }
    }

    size_t write_len = len;
    bool injected = false;
    if (sim->fail_program_armed) {
        write_len = sim->fail_program_after < len ? sim->fail_program_after : len;
        sim->fail_program_armed = false;
        injected = true;
    }

    for (size_t i = 0; i < write_len; i++) {
        sim->bytes[offset + i] &= in[i];
    }
    sim->program_count++;
    return !injected;
}

bool mstore_nor_sim_erase(mstore_nor_sim_t *sim, size_t offset, size_t len) {
    if (sim == NULL || !range_ok(sim, offset, len)) {
        return false;
    }
    if (len == 0 || offset % sim->erase_size != 0 || len % sim->erase_size != 0) {
        return false;
    }

    if (sim->fail_erase_armed) {
        sim->fail_erase_armed = false;
        return false;
    }
    memset(sim->bytes + offset, 0xFF, len);
    sim->erase_count++;
    return true;
}

void mstore_nor_sim_fail_program_after(mstore_nor_sim_t *sim, size_t bytes_to_write) {
    sim->fail_program_armed = true;
    sim->fail_program_after = bytes_to_write;
}

void mstore_nor_sim_fail_program_now(mstore_nor_sim_t *sim) {
    mstore_nor_sim_fail_program_after(sim, 0);
}

void mstore_nor_sim_fail_erase(mstore_nor_sim_t *sim) {
    sim->fail_erase_armed = true;
}

void mstore_nor_sim_poke(mstore_nor_sim_t *sim, size_t offset, uint8_t value) {
    if (offset < sim->size) {
        sim->bytes[offset] = value;
    }
}

size_t mstore_nor_sim_program_count(const mstore_nor_sim_t *sim) {
    return sim->program_count;
}

size_t mstore_nor_sim_erase_count(const mstore_nor_sim_t *sim) {
    return sim->erase_count;
}

bool mstore_nor_sim_save(const mstore_nor_sim_t *sim, const char *path) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const size_t written = fwrite(sim->bytes, 1, sim->size, file);
    const bool ok = written == sim->size;
    fclose(file);
    return ok;
}

mstore_nor_sim_t *mstore_nor_sim_load(const char *path, size_t erase_size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long end = ftell(file);
    if (end <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    mstore_nor_sim_t *sim = mstore_nor_sim_create((size_t)end, erase_size);
    if (sim == NULL) {
        fclose(file);
        return NULL;
    }
    const size_t read = fread(sim->bytes, 1, sim->size, file);
    fclose(file);
    if (read != sim->size) {
        mstore_nor_sim_destroy(sim);
        return NULL;
    }
    return sim;
}
