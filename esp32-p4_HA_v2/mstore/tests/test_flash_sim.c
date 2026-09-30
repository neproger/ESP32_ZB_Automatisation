#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nor_sim.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void check_all(uint8_t *buf, size_t len, uint8_t value) {
    for (size_t i = 0; i < len; i++) {
        CHECK(buf[i] == value);
    }
}

static void test_nor_constraints(mstore_nor_sim_t *sim) {
    uint8_t buf[8];

    CHECK(mstore_nor_sim_read(sim, 0, buf, sizeof(buf)));
    check_all(buf, sizeof(buf), 0xFF);

    const uint8_t zeros[4] = {0x00, 0x00, 0x00, 0x00};
    CHECK(mstore_nor_sim_program(sim, 0, zeros, sizeof(zeros)));
    CHECK(mstore_nor_sim_read(sim, 0, buf, sizeof(zeros)));
    check_all(buf, sizeof(zeros), 0x00);

    /* 0 -> 1 без erase запрещено, байты не меняются */
    const uint8_t ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!mstore_nor_sim_program(sim, 0, ones, sizeof(ones)));
    CHECK(mstore_nor_sim_read(sim, 0, buf, sizeof(ones)));
    check_all(buf, sizeof(ones), 0x00);

    /* erase: выравнивание и кратность */
    CHECK(!mstore_nor_sim_erase(sim, 1, 1024));
    CHECK(!mstore_nor_sim_erase(sim, 0, 100));
    CHECK(!mstore_nor_sim_erase(sim, 0, 8192));
    CHECK(mstore_nor_sim_erase_count(sim) == 0);

    CHECK(mstore_nor_sim_erase(sim, 0, 1024));
    CHECK(mstore_nor_sim_erase_count(sim) == 1);
    CHECK(mstore_nor_sim_read(sim, 0, buf, sizeof(buf)));
    check_all(buf, sizeof(buf), 0xFF);

    /* запись нескольких байт и повторная запись только 1 -> 0 */
    const uint8_t nibble[2] = {0x0F, 0x0F};
    CHECK(mstore_nor_sim_program(sim, 16, nibble, sizeof(nibble)));
    const uint8_t high[1] = {0xF0};
    CHECK(!mstore_nor_sim_program(sim, 16, high, sizeof(high)));

    /* out of range */
    CHECK(!mstore_nor_sim_read(sim, 4096, buf, 1));
    CHECK(!mstore_nor_sim_program(sim, 4096, zeros, 1));
}

static void test_fault_injection(mstore_nor_sim_t *sim) {
    uint8_t buf[8];
    const uint8_t zeros[4] = {0, 0, 0, 0};

    CHECK(mstore_nor_sim_erase(sim, 2048, 1024));

    /* partial program: 2 байта записаны, дальше отказ */
    mstore_nor_sim_fail_program_after(sim, 2);
    CHECK(!mstore_nor_sim_program(sim, 2048, zeros, sizeof(zeros)));
    CHECK(mstore_nor_sim_read(sim, 2048, buf, 4));
    CHECK(buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0xFF && buf[3] == 0xFF);

    /* fault одноразовый: следующая запись проходит */
    CHECK(mstore_nor_sim_program(sim, 2052, zeros, sizeof(zeros)));

    /* fail now: ничего не записано */
    mstore_nor_sim_fail_program_now(sim);
    CHECK(!mstore_nor_sim_program(sim, 2060, zeros, sizeof(zeros)));
    CHECK(mstore_nor_sim_read(sim, 2060, buf, 1));
    CHECK(buf[0] == 0xFF);

    /* fail erase: байты не меняются, затем erase проходит */
    CHECK(mstore_nor_sim_program(sim, 3072, zeros, sizeof(zeros)));
    mstore_nor_sim_fail_erase(sim);
    CHECK(!mstore_nor_sim_erase(sim, 3072, 1024));
    CHECK(mstore_nor_sim_read(sim, 3072, buf, 1));
    CHECK(buf[0] == 0x00);
    CHECK(mstore_nor_sim_erase(sim, 3072, 1024));
    CHECK(mstore_nor_sim_read(sim, 3072, buf, 1));
    CHECK(buf[0] == 0xFF);

    /* poke: искажение байта */
    mstore_nor_sim_poke(sim, 4000, 0x0F);
    CHECK(mstore_nor_sim_read(sim, 4000, buf, 1));
    CHECK(buf[0] == 0x0F);
}

static void test_reboot_roundtrip(mstore_nor_sim_t *sim) {
    const char *path = "nor_sim_roundtrip.bin";
    const uint8_t marker[3] = {0x00, 0x0F, 0x00};
    CHECK(mstore_nor_sim_program(sim, 100, marker, sizeof(marker)));

    CHECK(mstore_nor_sim_save(sim, path));
    mstore_nor_sim_t *reopened = mstore_nor_sim_load(path, mstore_nor_sim_erase_size(sim));
    CHECK(reopened != NULL);
    CHECK(mstore_nor_sim_size(reopened) == mstore_nor_sim_size(sim));

    uint8_t a[3], b[3];
    CHECK(mstore_nor_sim_read(sim, 100, a, sizeof(a)));
    CHECK(mstore_nor_sim_read(reopened, 100, b, sizeof(b)));
    CHECK(memcmp(a, b, sizeof(a)) == 0);

    mstore_nor_sim_destroy(reopened);
    CHECK(remove(path) == 0);
}

int main(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(4096, 1024);
    CHECK(sim != NULL);
    CHECK(mstore_nor_sim_size(sim) == 4096);
    CHECK(mstore_nor_sim_erase_size(sim) == 1024);

    test_nor_constraints(sim);
    test_fault_injection(sim);
    test_reboot_roundtrip(sim);

    mstore_nor_sim_destroy(sim);
    printf("test_flash_sim: OK\n");
    return 0;
}
