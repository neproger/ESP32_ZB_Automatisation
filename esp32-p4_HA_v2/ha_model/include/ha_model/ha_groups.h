#pragma once

/*
 * Формы экранов Display (docs/clients/DISPLAY.md): `group` — экран/дашборд,
 * `group_item` — виджет на экране. Формы, а не логика: только структуры и числа.
 * Заголовок линкуют сервисы и bootstrap; Domain его не включает и этих имён не знает.
 *
 * Плитка экрана ссылается на логическую Entity (endpoint), а не на одно свойство:
 * форму виджетов задаёт capability Entity (как в HA v1 — «виджеты от типа девайса»).
 * Ключ `group_item` — составной `(group_id, entity)`: одна Entity может стоять на
 * нескольких экранах.
 *
 * `title` у экрана и у элемента — пользовательские подписи, независимые от имени
 * устройства. Это canonical state (пользователь меняет осознанно), а не operational
 * metadata; версии и временных меток в записи нет — они живут в meta хранилища
 * (docs/RECORD_MODEL.md §2.2, §4).
 */

#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_entity.h" /* ha_entity_id_t */

#ifdef __cplusplus
extern "C" {
#endif

#define HA_GROUP_TITLE_MAX      32
#define HA_GROUP_ITEM_TITLE_MAX 32

/* Экран: ключ — числовой id (как у automation), payload — пользовательский заголовок. */
typedef struct {
    uint64_t id;
} ha_group_key_t;

typedef struct {
    char title[HA_GROUP_TITLE_MAX];
} ha_group_record_t;

/*
 * Плитка на экране: ссылка на логическую Entity + порядок + подпись. Ключ — составной
 * `(group_id, entity)`; форму виджетов выбирает потребитель по capability Entity
 * (properties/actions), без ZCL. `order` задаёт порядок плиток; `title` — подпись
 * (не зависит от имени Entity).
 */
typedef struct {
    uint64_t group_id;
    ha_entity_id_t entity;
} ha_group_item_key_t;

typedef struct {
    uint16_t order;
    uint8_t reserved[2];
    char title[HA_GROUP_ITEM_TITLE_MAX];
} ha_group_item_record_t;

/*
 * Плитка-переключатель автоматизации на экране: ключ (экран, правило). Запись — та же
 * `ha_group_item_record_t` (order + title): плитке автоматизации тоже нужна подпись и место.
 */
typedef struct {
    uint64_t group_id;
    uint64_t automation_id;
} ha_group_automation_key_t;

#ifdef __cplusplus
static_assert(sizeof(ha_group_key_t) == 8, "ha_group_key_t: неожиданный размер");
static_assert(sizeof(ha_group_record_t) == 32, "ha_group_record_t: неожиданный размер");
static_assert(sizeof(ha_group_item_key_t) == 16, "ha_group_item_key_t: неожиданный размер");
static_assert(offsetof(ha_group_item_key_t, entity) == 8, "ha_group_item_key_t: layout");
static_assert(offsetof(ha_group_item_record_t, title) == 4, "ha_group_item_record_t: layout");
static_assert(sizeof(ha_group_item_record_t) == 36, "ha_group_item_record_t: неожиданный размер");
static_assert(sizeof(ha_group_automation_key_t) == 16, "ha_group_automation_key_t: неожиданный размер");
static_assert(offsetof(ha_group_automation_key_t, automation_id) == 8, "ha_group_automation_key_t: layout");
#else
_Static_assert(sizeof(ha_group_key_t) == 8, "ha_group_key_t: неожиданный размер");
_Static_assert(sizeof(ha_group_record_t) == 32, "ha_group_record_t: неожиданный размер");
_Static_assert(sizeof(ha_group_item_key_t) == 16, "ha_group_item_key_t: неожиданный размер");
_Static_assert(offsetof(ha_group_item_key_t, entity) == 8, "ha_group_item_key_t: layout");
_Static_assert(offsetof(ha_group_item_record_t, title) == 4, "ha_group_item_record_t: layout");
_Static_assert(sizeof(ha_group_item_record_t) == 36, "ha_group_item_record_t: неожиданный размер");
_Static_assert(sizeof(ha_group_automation_key_t) == 16, "ha_group_automation_key_t: неожиданный размер");
_Static_assert(offsetof(ha_group_automation_key_t, automation_id) == 8, "ha_group_automation_key_t: layout");
#endif

#ifdef __cplusplus
}
#endif
