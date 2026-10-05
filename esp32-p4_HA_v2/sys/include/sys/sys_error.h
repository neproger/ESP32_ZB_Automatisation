#pragma once

/*
 * Единая модель ошибки системы (docs/ERRORS.md).
 *
 * Ошибка создаётся слоем, в котором возникла, и проходит через остальные слои без
 * перекодирования: промежуточный слой не знает об ошибке больше источника.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Кто создал ошибку. Это диагностика, а не логика: смысл определяет code, и вызывающий
 * сравнивает именно его, не разбирая, какой слой ошибку породил.
 */
typedef enum {
    SYS_LAYER_NONE = 0,
    SYS_LAYER_MSTORE,
    SYS_LAYER_DOMAIN,
    SYS_LAYER_ZIGBEE,
    SYS_LAYER_AUTOMATION,
    SYS_LAYER_WEB,
    SYS_LAYER_SYSTEM,
    SYS_LAYER_WIFI,
} sys_layer_t;

/*
 * Общее пространство кодов: одно значение означает одно и то же в любом слое.
 * Коды, осмысленные только внутри одного слоя, начинаются с SYS_CODE_LAYER_BASE.
 */
typedef enum {
    SYS_CODE_OK = 0,
    SYS_CODE_NOT_FOUND,
    SYS_CODE_ALREADY_EXISTS,
    SYS_CODE_STALE,
    SYS_CODE_NO_SPACE,
    SYS_CODE_NO_MEM,
    SYS_CODE_INVALID_ARG,
    SYS_CODE_INVALID_SIZE,
    SYS_CODE_INVALID_STATE,
    SYS_CODE_INVARIANT_FAILED,
    SYS_CODE_IO,
    SYS_CODE_CORRUPT,
    SYS_CODE_OVERFLOW,
    SYS_CODE_BUSY,

    SYS_CODE_LAYER_BASE = 1000,
} sys_code_t;

typedef struct {
    uint16_t layer;
    uint16_t code;
} sys_error_t;

#define SYS_OK ((sys_error_t){0u, (uint16_t)SYS_CODE_OK})

static inline sys_error_t sys_error_make(sys_layer_t layer, sys_code_t code)
{
    const sys_error_t err = {(uint16_t)layer, (uint16_t)code};
    return err;
}

static inline bool sys_ok(sys_error_t err)
{
    return err.code == (uint16_t)SYS_CODE_OK;
}

static inline bool sys_failed(sys_error_t err)
{
    return err.code != (uint16_t)SYS_CODE_OK;
}

/* Ожидаемый outcome распознаётся по коду, без перекодирования ошибки. */
static inline bool sys_is(sys_error_t err, sys_code_t code)
{
    return err.code == (uint16_t)code;
}

#ifdef __cplusplus
}
#endif
