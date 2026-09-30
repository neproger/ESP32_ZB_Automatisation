#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-модель NOR-региона для разработки durable format без реальной P4.
 *
 * Ограничения NOR, которые моделируются:
 *   - erased byte = 0xFF;
 *   - program только 1 -> 0 (попытка 0 -> 1 — ошибка, байты не меняются);
 *   - erase возвращает блок в 0xFF и требует выравнивания по erase_size;
 *   - ограниченный регион (offset + len внутри size).
 *
 * Fault injection — одноразовая: срабатывает на следующей операции и снимается.
 * "Reboot" моделируется сохранением буфера и повторным открытием поверх него
 * (mstore_nor_sim_save / mstore_nor_sim_load).
 */

typedef struct mstore_nor_sim mstore_nor_sim_t;

mstore_nor_sim_t *mstore_nor_sim_create(size_t size, size_t erase_size);
void mstore_nor_sim_destroy(mstore_nor_sim_t *sim);

size_t mstore_nor_sim_size(const mstore_nor_sim_t *sim);
size_t mstore_nor_sim_erase_size(const mstore_nor_sim_t *sim);

/* Устройство. false — нарушение ограничений NOR или сработавшая инжекция. */
bool mstore_nor_sim_read(const mstore_nor_sim_t *sim, size_t offset, void *dst, size_t len);
bool mstore_nor_sim_program(mstore_nor_sim_t *sim, size_t offset, const void *src, size_t len);
bool mstore_nor_sim_erase(mstore_nor_sim_t *sim, size_t offset, size_t len);

/* Fault injection. */
void mstore_nor_sim_fail_program_after(mstore_nor_sim_t *sim, size_t bytes_to_write);
void mstore_nor_sim_fail_program_now(mstore_nor_sim_t *sim);
void mstore_nor_sim_fail_erase(mstore_nor_sim_t *sim);

/* Прямое искажение байта (bit rot / partial state). */
void mstore_nor_sim_poke(mstore_nor_sim_t *sim, size_t offset, uint8_t value);

size_t mstore_nor_sim_program_count(const mstore_nor_sim_t *sim);
size_t mstore_nor_sim_erase_count(const mstore_nor_sim_t *sim);

/* Persistence для моделирования reboot across process runs. */
bool mstore_nor_sim_save(const mstore_nor_sim_t *sim, const char *path);
mstore_nor_sim_t *mstore_nor_sim_load(const char *path, size_t erase_size);

#ifdef __cplusplus
}
#endif
