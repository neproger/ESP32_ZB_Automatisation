#include "nor_sim_device.h"

static bool sim_read(void *ctx, size_t offset, void *dst, size_t len) {
    return mstore_nor_sim_read((mstore_nor_sim_t *)ctx, offset, dst, len);
}

static bool sim_program(void *ctx, size_t offset, const void *src, size_t len) {
    return mstore_nor_sim_program((mstore_nor_sim_t *)ctx, offset, src, len);
}

static bool sim_erase(void *ctx, size_t offset, size_t len) {
    return mstore_nor_sim_erase((mstore_nor_sim_t *)ctx, offset, len);
}

static const mstore_flash_device_ops_t NOR_SIM_OPS = {
    .read = sim_read,
    .program = sim_program,
    .erase = sim_erase,
};

void nor_sim_device_init(nor_sim_device_t *device, mstore_nor_sim_t *sim) {
    device->base.ops = &NOR_SIM_OPS;
    device->base.ctx = sim;
    device->base.size = mstore_nor_sim_size(sim);
    device->base.erase_size = mstore_nor_sim_erase_size(sim);
}
