#pragma once

/*
 * Формы команд (docs/RECORD_MODEL.md §9, docs/domain/COMMANDS.md §7) — transport-agnostic.
 * Raw ZCL-команда (ha_zb_command_t) здесь НЕ живёт: она принадлежит мосту внутри zigbee
 * (zigbee/zb_command.h). Здесь — только Domain-дискриминаторы и payload'ы.
 */

#include <stdint.h>

#include "ha_model/ha_entity.h"
#include "ha_model/ha_properties.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Был HA_CMD_ZIGBEE_CLUSTER(1): удалён (Шаг C, C2) — raw ZCL вне общего командного шины. */
/* Был HA_CMD_DEVICE_REMOVE(2): удалён (Step F) — удаление только через Device service. */

/*
 * Запустить/остановить поиск новых устройств (args = uint8_t seconds, 0 — стоп). Generic:
 * конкретный adapter решает, что это (Zigbee — permit join, Matter — commissioning и т.п.).
 */
#define HA_CMD_DISCOVERY 3u

/* Wi-Fi: запустить скан (args нет); результат — сущность HA_ENTITY_WIFI_SCAN. */
#define HA_CMD_WIFI_SCAN 4u

/* Wi-Fi: подключиться к точке (args = ha_wifi_connect_args_t, ha_wifi.h). */
#define HA_CMD_WIFI_CONNECT 5u

/*
 * Семантическая команда по entity (args = ha_property_command_t, Шаг A, A3.2). Domain
 * несёт её как opaque payload; executor транспорта резолвит entity→физический адрес и
 * кодирует через мост. Потребителю (Automation/Web/Display) не нужны uid/cluster/attr.
 */
#define HA_CMD_PROPERTY 6u

/*
 * Переименование логической Entity (args = ha_entity_rename_t, Шаг A, A6.1). Меняет
 * Entity.name напрямую; adapter не участвует — имя принадлежит Entity, а не транспорту.
 */
#define HA_CMD_ENTITY_RENAME 7u

/*
 * Семантическая команда: цель — (entity_id, property_id), намерение — action + value.
 * HA_ENTITY_ID_SOURCE/NONE сюда не попадают: вызывающий резолвит источник до постинга.
 */
typedef struct {
    ha_entity_id_t entity;
    ha_property_id_t property;
    uint8_t action; /* ha_action_id_t */
    uint8_t reserved[3];
    ha_command_value_t value;
} ha_property_command_t;

/* Аргумент HA_CMD_ENTITY_RENAME. */
typedef struct {
    ha_entity_id_t entity;
    char name[HA_ENTITY_NAME_MAX];
} ha_entity_rename_t;

#ifdef __cplusplus
static_assert(sizeof(ha_property_command_t) == 28, "ha_property_command_t: неожиданный размер");
static_assert(sizeof(ha_entity_rename_t) == 40, "ha_entity_rename_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
