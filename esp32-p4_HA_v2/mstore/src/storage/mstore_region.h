#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mstore/mstore_types.h"
#include "storage/mstore_flash_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Region Manager: владелец раздела (partition).
 *
 * Отвечает за directory регионов и правило bind/release; не знает про слоты,
 * записи и generation. Размер региона считает FLASH backend — он владеет форматом.
 *
 * Результат bind — region view: derived device, который транслирует offset в
 * абсолютный и ограничивает доступ границами региона. FLASH backend работает с
 * ним как с обычным NOR-устройством и остаётся единственным владельцем формата
 * внутри региона.
 *
 * Ограничения v1:
 *   - directory bounded: MSTORE_REGION_MAX_ENTRIES записей, bump-аллокатор;
 *   - erase_size должен вмещать слот directory (MSTORE_REGION_SLOT_BYTES);
 *   - регион не растёт: смена геометрии даёт INVALID_SIZE;
 *   - bind/release не синхронизируются между задачами (concurrency — отдельный шаг).
 */

#define MSTORE_REGION_MAX_ENTRIES 16u
#define MSTORE_REGION_HEADER_SIZE 16u
#define MSTORE_REGION_ENTRY_SIZE 32u
#define MSTORE_REGION_SLOT_BYTES (MSTORE_REGION_HEADER_SIZE + MSTORE_REGION_MAX_ENTRIES * MSTORE_REGION_ENTRY_SIZE)

typedef struct {
    const mstore_flash_device_t *parent;
    size_t base_offset;
    size_t size;
} mstore_region_view_ctx_t;

typedef struct {
    mstore_flash_device_t device;
    mstore_region_view_ctx_t ctx;
} mstore_region_view_t;

/*
 * Резервирует регион под persist_key и отдаёт view на него. Повторный bind того же
 * ключа без release — INVALID_STATE. Свободное место под регион считается от конца
 * directory; запись directory пишется до того, как backend начнёт писать в регион.
 */
sys_error_t mstore_region_bind(const char *persist_key, size_t capacity, size_t key_size,
                                size_t payload_size, mstore_region_view_t *out_view);

/* Снимает признак занятости. Вызывается на закрытии storage: reopen — разрешённый сценарий. */
void mstore_region_release(const char *persist_key);

/* Положение региона в разделе. Для тестов, которым нужно искажать байты внутри региона. */
sys_error_t mstore_region_lookup(const char *persist_key, size_t *out_offset, size_t *out_size);

#ifdef __cplusplus
}
#endif
