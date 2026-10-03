#pragma once

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "sys/sys_error.h"
#include "zigbee/zigbee_interview.h"

/*
 * Zigbee service (docs/services/ZIGBEE.md).
 *
 * Сервис — адаптер к Zigbee-миру: принимает репорты, кладёт состояние в Domain,
 * исполняет команды. Источник истины — Domain, а не сервис.
 */

/* Репорт в терминах ZCL: то, что отдаёт радио/шлюз, без интерпретации. */
typedef struct {
    ha_device_uid_t device_uid;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t endpoint;
    uint8_t zcl_type; /* HA_ZB_TYPE_* из ha_zigbee.h */
    uint32_t raw;     /* значение в кодировке ZCL (битовая копия для float) */
} zigbee_report_t;

/*
 * Событие от устройства: команда/нажатие, из которого состояние не следует
 * (docs/services/ZIGBEE.md §4). Публикуется как EVENT с payload.
 */
#define ZIGBEE_EVENT_PAYLOAD_MAX 16

typedef struct {
    ha_device_uid_t device_uid;
    uint16_t cluster_id;
    uint8_t command_id;
    uint8_t endpoint;
    uint8_t payload_length;
    uint8_t payload[ZIGBEE_EVENT_PAYLOAD_MAX];
} zigbee_event_t;

/* Поднимает задачу сервиса. Вызывается из bootstrap после domain_init(). */
sys_error_t zigbee_start(domain_t *domain);

/*
 * Принять репорт. Для вызывающего это fire-and-forget: репорт ставится в очередь
 * задачи сервиса, состояние появится в Domain позже. BUSY — очередь заполнена.
 */
sys_error_t zigbee_submit_report(const zigbee_report_t *report);

/*
 * Принять результат интервью. Fire-and-forget: интервью идёт в задаче радио, а запись
 * устройства и топологии в Domain — в задаче сервиса (docs/services/ZIGBEE.md §6).
 * BUSY — очередь интервью заполнена.
 */
sys_error_t zigbee_submit_interview(const zigbee_interview_result_t *result);

/*
 * Принять событие от устройства. Fire-and-forget: EVENT в Domain пишет задача сервиса
 * (docs/services/ZIGBEE.md §6). BUSY — очередь событий заполнена.
 */
sys_error_t zigbee_submit_event(const zigbee_event_t *event);

/*
 * Устройство ушло из сети. Fire-and-forget: снятие записей в Domain идёт в задаче
 * сервиса (docs/services/ZIGBEE.md §6). BUSY — очередь заполнена.
 */
sys_error_t zigbee_submit_leave(ha_device_uid_t device_uid);
