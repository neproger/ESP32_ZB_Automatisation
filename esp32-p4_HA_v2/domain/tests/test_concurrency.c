#include "domain/domain.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

enum { TYPE_A = 1 };

/*
 * Сценарий проверяет сериализацию mutation path: writers гоняют put/remove на
 * ОБЩЕМ маленьком наборе ключей, поэтому слоты постоянно освобождаются и
 * переиспользуются. Без lock'а Domain последовательность
 * slot_find → slot_meta → slot_update рвётся и put возвращает STALE.
 */
#define WRITER_COUNT 3
#define ROUNDS 3000
#define SHARED_KEYS 16

typedef struct {
    uint32_t id;
    uint8_t endpoint;
} test_key_t;

typedef struct {
    uint32_t value;
    uint32_t reserved;
} test_record_t;

typedef struct {
    domain_t *domain;
    volatile int stale_seen;
} writer_arg_t;

static test_key_t key_of(uint32_t id)
{
    test_key_t key = {0};
    key.id = id;
    key.endpoint = 1;
    return key;
}

static void *writer_main(void *raw)
{
    writer_arg_t *arg = (writer_arg_t *)raw;
    bool changed = false;
    test_record_t record = {0};

    for (size_t round = 0; round < ROUNDS; ++round) {
        for (size_t i = 0; i < SHARED_KEYS; ++i) {
            const test_key_t key = key_of((uint32_t)i);
            record.value = (uint32_t)(round + i);

            const domain_err_t put_err =
                domain_entity_put(arg->domain, TYPE_A, &key, &record, NULL, &changed);
            if (put_err == DOMAIN_STALE) {
                arg->stale_seen = 1;
            } else if (put_err != DOMAIN_OK) {
                return (void *)1;
            }

            const domain_err_t remove_err =
                domain_entity_remove(arg->domain, TYPE_A, &key, NULL);
            if (remove_err != DOMAIN_OK && remove_err != DOMAIN_NOT_FOUND) {
                return (void *)1;
            }
        }
    }
    return NULL;
}

static void *reader_main(void *raw)
{
    domain_t *domain = (domain_t *)raw;
    test_record_t out = {0};

    for (size_t round = 0; round < ROUNDS; ++round) {
        const test_key_t key = key_of((uint32_t)(round % SHARED_KEYS));
        const domain_err_t err = domain_entity_get(domain, TYPE_A, &key, &out);
        if (err != DOMAIN_OK && err != DOMAIN_NOT_FOUND) {
            return (void *)1;
        }
    }
    return NULL;
}

#ifdef _WIN32

static DWORD WINAPI win_writer(LPVOID arg)
{
    return (DWORD)(size_t)writer_main(arg);
}

static DWORD WINAPI win_reader(LPVOID arg)
{
    return (DWORD)(size_t)reader_main(arg);
}

static void test_serialized_mutation_path(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);

    domain_entity_desc_t desc = {0};
    desc.type = TYPE_A;
    desc.key_size = sizeof(test_key_t);
    desc.payload_size = sizeof(test_record_t);
    desc.capacity = SHARED_KEYS + 4;
    desc.backing = DOMAIN_BACKING_RAM;
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    writer_arg_t args[WRITER_COUNT] = {0};
    HANDLE threads[WRITER_COUNT + 1] = {NULL};

    for (size_t i = 0; i < WRITER_COUNT; ++i) {
        args[i].domain = &domain;
        threads[i] = CreateThread(NULL, 0, win_writer, &args[i], 0, NULL);
        CHECK(threads[i] != NULL);
    }
    threads[WRITER_COUNT] = CreateThread(NULL, 0, win_reader, &domain, 0, NULL);
    CHECK(threads[WRITER_COUNT] != NULL);

    WaitForMultipleObjects((DWORD)(WRITER_COUNT + 1), threads, TRUE, INFINITE);

    DWORD codes[WRITER_COUNT + 1] = {0};
    for (size_t i = 0; i < WRITER_COUNT + 1; ++i) {
        GetExitCodeThread(threads[i], &codes[i]);
        CloseHandle(threads[i]);
    }
    for (size_t i = 0; i < WRITER_COUNT + 1; ++i) {
        CHECK(codes[i] == 0);
    }
    for (size_t i = 0; i < WRITER_COUNT; ++i) {
        CHECK(args[i].stale_seen == 0);
    }

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

#else

static void test_serialized_mutation_path(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);

    domain_entity_desc_t desc = {0};
    desc.type = TYPE_A;
    desc.key_size = sizeof(test_key_t);
    desc.payload_size = sizeof(test_record_t);
    desc.capacity = SHARED_KEYS + 4;
    desc.backing = DOMAIN_BACKING_RAM;
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    pthread_t threads[WRITER_COUNT + 1] = {0};
    writer_arg_t args[WRITER_COUNT] = {0};

    for (size_t i = 0; i < WRITER_COUNT; ++i) {
        args[i].domain = &domain;
        CHECK(pthread_create(&threads[i], NULL, writer_main, &args[i]) == 0);
    }
    CHECK(pthread_create(&threads[WRITER_COUNT], NULL, reader_main, &domain) == 0);

    void *status = NULL;
    for (size_t i = 0; i < WRITER_COUNT + 1; ++i) {
        pthread_join(threads[i], &status);
        CHECK(status == NULL);
    }
    for (size_t i = 0; i < WRITER_COUNT; ++i) {
        CHECK(args[i].stale_seen == 0);
    }

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

#endif

int main(void)
{
    test_serialized_mutation_path();

    if (g_failures != 0) {
        printf("test_concurrency: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_concurrency: OK\n");
    return 0;
}
