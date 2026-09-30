#pragma once

#include "nor_sim.h"
#include "storage/mstore_flash_device.h"

/* Adapter: nor_sim как internal mstore_flash_device_t. */
typedef struct {
    mstore_flash_device_t base;
} nor_sim_device_t;

void nor_sim_device_init(nor_sim_device_t *device, mstore_nor_sim_t *sim);
