#include "domain/domain.h"

#include <stdio.h>

#include "domain_internal.h"

/*
 * Команды: маршрутизация напрямую executor'у и COMMAND_SENT только после передачи
 * (docs/domain/COMMANDS.md).
 */

static int failures = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

#define CMD_SET_LEVEL 1
#define CMD_UNKNOWN 99

typedef struct {
    uint8_t level;
} set_level_args_t;

typedef struct {
    bool accept;
    size_t count;
    domain_event_t events[16];
} inbox_t;

static bool inbox_push(const domain_event_t *event, void *ctx)
{
    inbox_t *inbox = (inbox_t *)ctx;
    if (!inbox->accept) {
        return false;
    }
    if (inbox->count < 16) {
        inbox->events[inbox->count] = *event;
    }
    inbox->count++;
    return true;
}

typedef struct {
    size_t calls;
    uint32_t last_type;
    uint8_t last_level;
    sys_error_t result;
} executor_state_t;

static sys_error_t set_level_executor(domain_command_t type, const void *args, size_t args_size,
                                      void *ctx)
{
    executor_state_t *seen = (executor_state_t *)ctx;
    seen->calls++;
    seen->last_type = type;
    if (args != NULL && args_size == sizeof(set_level_args_t)) {
        seen->last_level = ((const set_level_args_t *)args)->level;
    }
    return seen->result;
}

static void command_reaches_executor(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, 4, 32)));

    executor_state_t seen = {.result = SYS_OK};
    CHECK(sys_ok(domain_register_command(&domain, CMD_SET_LEVEL, set_level_executor, &seen)));

    const set_level_args_t args = {.level = 32};
    CHECK(sys_ok(domain_post(&domain, CMD_SET_LEVEL, &args, sizeof(args), NULL)));
    CHECK(seen.calls == 1);
    CHECK(seen.last_type == CMD_SET_LEVEL);
    CHECK(seen.last_level == 32);

    /* Повторная регистрация того же типа — ошибка, а не замена исполнителя. */
    CHECK(sys_is(domain_register_command(&domain, CMD_SET_LEVEL, set_level_executor, &seen),
                 SYS_CODE_INVALID_STATE));

    /* Исполнителя нет — команда не молча теряется. */
    CHECK(sys_is(domain_post(&domain, CMD_UNKNOWN, &args, sizeof(args), NULL),
                 SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void command_sent_is_journaled(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, 4, 32)));

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = {0};
    desc.try_push = inbox_push;
    desc.ctx = &inbox;
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(&domain, &desc, &sub)));

    executor_state_t seen = {.result = SYS_OK};
    CHECK(sys_ok(domain_register_command(&domain, CMD_SET_LEVEL, set_level_executor, &seen)));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    meta.value.type = (uint8_t)DOMAIN_VALUE_U32;
    meta.value.v.u32 = 32;

    const set_level_args_t args = {.level = 32};
    CHECK(sys_ok(domain_post(&domain, CMD_SET_LEVEL, &args, sizeof(args), &meta)));

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(&domain, &delivered)));
    CHECK(delivered == 1);
    CHECK(inbox.events[0].kind == DOMAIN_FACT_COMMAND_SENT);
    CHECK(inbox.events[0].op == DOMAIN_OP_COMMAND);
    CHECK(inbox.events[0].source == DOMAIN_SOURCE_UI);
    CHECK(inbox.events[0].value.v.u32 == 32);

    CHECK(sys_ok(domain_unsubscribe(&domain, sub)));
    CHECK(sys_ok(domain_deinit(&domain)));
}

static void rejected_command_writes_no_fact(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, 4, 32)));

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = {0};
    desc.try_push = inbox_push;
    desc.ctx = &inbox;
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(&domain, &desc, &sub)));

    executor_state_t seen = {.result = domain_fail(SYS_CODE_BUSY)};
    CHECK(sys_ok(domain_register_command(&domain, CMD_SET_LEVEL, set_level_executor, &seen)));

    const set_level_args_t args = {.level = 5};
    CHECK(sys_is(domain_post(&domain, CMD_SET_LEVEL, &args, sizeof(args), NULL), SYS_CODE_BUSY));
    CHECK(seen.calls == 1);

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(&domain, &delivered)));
    CHECK(delivered == 0); /* COMMAND_SENT означает «передано», а не «попытались» */

    CHECK(sys_ok(domain_unsubscribe(&domain, sub)));
    CHECK(sys_ok(domain_deinit(&domain)));
}

int main(void)
{
    command_reaches_executor();
    command_sent_is_journaled();
    rejected_command_writes_no_fact();

    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("test_commands: OK\n");
    return 0;
}
