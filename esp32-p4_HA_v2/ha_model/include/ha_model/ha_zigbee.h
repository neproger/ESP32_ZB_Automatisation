#pragma once

/*
 * ha_model / Zigbee: общий словарь идентификаторов (docs/RECORD_MODEL.md §9).
 *
 * Это словарь, а не слой: только стабильные числа, никакой логики, никаких
 * зависимостей. Его линкуют сервисы и bootstrap; Domain его не видит и не включает.
 *
 * Источник значений: Zigbee Cluster Library (ZCL6, 07-5123-06). Значения
 * кластеров/команд/атрибутов/типов определены спецификацией и не зависят от SDK.
 * Имена SDK v2.x (esp-zigbee-lib, пространство EZB_ZCL_*, заголовки вида
 * zcl/cluster/<cluster>.h) — только способ сверить числа при появлении Zigbee-слоя:
 * расхождение означает ошибку здесь, а не в SDK.
 *
 * Словарь не содержит: формирование команд, валидацию ZCL-аргументов, масштабирование
 * единиц (0.01 °C и т. п.) — это поведение Zigbee-сервиса, а не общий словарь.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Идентичность устройства — IEEE EUI-64. Stable identity: не зависит от сети,
 * остаётся той же после перепривязки и смены короткого адреса. Short address здесь
 * не определяется сознательно: это текущее сетевое состояние, а не identity, и
 * разрешает его Zigbee-сервис.
 */
typedef uint64_t ha_device_uid_t;

/* Профиль Zigbee Home Automation. */
#define HA_ZB_PROFILE_HA 0x0104u

/* Кластеры (ZCL cluster id). */
#define HA_ZB_CLUSTER_BASIC 0x0000u
#define HA_ZB_CLUSTER_POWER_CONFIG 0x0001u
#define HA_ZB_CLUSTER_IDENTIFY 0x0003u
#define HA_ZB_CLUSTER_GROUPS 0x0004u
#define HA_ZB_CLUSTER_SCENES 0x0005u
#define HA_ZB_CLUSTER_ON_OFF 0x0006u
#define HA_ZB_CLUSTER_ON_OFF_SWITCH_CONFIG 0x0007u
#define HA_ZB_CLUSTER_LEVEL_CONTROL 0x0008u
#define HA_ZB_CLUSTER_COLOR_CONTROL 0x0300u
#define HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT 0x0400u
#define HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT 0x0402u
#define HA_ZB_CLUSTER_RELATIVE_HUMIDITY 0x0405u
#define HA_ZB_CLUSTER_OCCUPANCY_SENSING 0x0406u
#define HA_ZB_CLUSTER_IAS_ZONE 0x0500u
#define HA_ZB_CLUSTER_METERING 0x0702u
#define HA_ZB_CLUSTER_ELECTRICAL_MEASUREMENT 0x0B04u

/* Foundation (global) команды: на них держатся репорты и чтение атрибутов. */
#define HA_ZB_CMD_READ_ATTRIBUTES 0x00u
#define HA_ZB_CMD_READ_ATTRIBUTES_RESPONSE 0x01u
#define HA_ZB_CMD_WRITE_ATTRIBUTES 0x02u
#define HA_ZB_CMD_WRITE_ATTRIBUTES_RESPONSE 0x04u
#define HA_ZB_CMD_CONFIGURE_REPORTING 0x06u
#define HA_ZB_CMD_CONFIGURE_REPORTING_RESPONSE 0x07u
#define HA_ZB_CMD_REPORT_ATTRIBUTES 0x0Au
#define HA_ZB_CMD_DEFAULT_RESPONSE 0x0Bu
#define HA_ZB_CMD_DISCOVER_ATTRIBUTES 0x0Cu
#define HA_ZB_CMD_DISCOVER_ATTRIBUTES_RESPONSE 0x0Du

/* Cluster-specific команды: On/Off, Level Control, Identify. */
#define HA_ZB_CMD_ON_OFF_OFF 0x00u
#define HA_ZB_CMD_ON_OFF_ON 0x01u
#define HA_ZB_CMD_ON_OFF_TOGGLE 0x02u
#define HA_ZB_CMD_ON_OFF_OFF_WITH_EFFECT 0x40u
#define HA_ZB_CMD_ON_OFF_ON_WITH_RECALL_GLOBAL_SCENE 0x41u
#define HA_ZB_CMD_ON_OFF_ON_WITH_TIMED_OFF 0x42u

#define HA_ZB_CMD_LEVEL_MOVE_TO_LEVEL 0x00u
#define HA_ZB_CMD_LEVEL_MOVE 0x01u
#define HA_ZB_CMD_LEVEL_STEP 0x02u
#define HA_ZB_CMD_LEVEL_STOP 0x03u
#define HA_ZB_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF 0x04u
#define HA_ZB_CMD_LEVEL_MOVE_WITH_ON_OFF 0x05u
#define HA_ZB_CMD_LEVEL_STEP_WITH_ON_OFF 0x06u
#define HA_ZB_CMD_LEVEL_STOP_WITH_ON_OFF 0x07u

#define HA_ZB_CMD_IDENTIFY_IDENTIFY 0x00u
#define HA_ZB_CMD_IDENTIFY_QUERY 0x01u

/* Атрибуты, нужные для идентификации состояния. */
#define HA_ZB_ATTR_ON_OFF_ON_OFF 0x0000u
#define HA_ZB_ATTR_ON_OFF_SWITCH_CONFIG_SWITCH_ACTIONS 0x0010u

#define HA_ZB_ATTR_LEVEL_CURRENT_LEVEL 0x0000u
#define HA_ZB_ATTR_LEVEL_REMAINING_TIME 0x0001u
#define HA_ZB_ATTR_LEVEL_ON_OFF_TRANSITION_TIME 0x0010u
#define HA_ZB_ATTR_LEVEL_ON_LEVEL 0x0011u

#define HA_ZB_ATTR_POWER_CONFIG_BATTERY_VOLTAGE 0x0020u
#define HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING 0x0021u

#define HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE 0x0000u
#define HA_ZB_ATTR_TEMPERATURE_MIN_MEASURED_VALUE 0x0001u
#define HA_ZB_ATTR_TEMPERATURE_MAX_MEASURED_VALUE 0x0002u
#define HA_ZB_ATTR_TEMPERATURE_TOLERANCE 0x0003u

#define HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE 0x0000u
#define HA_ZB_ATTR_HUMIDITY_TOLERANCE 0x0003u

#define HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE 0x0000u
#define HA_ZB_ATTR_OCCUPANCY_OCCUPANCY 0x0000u

#define HA_ZB_ATTR_COLOR_CURRENT_HUE 0x0000u
#define HA_ZB_ATTR_COLOR_CURRENT_SATURATION 0x0001u
#define HA_ZB_ATTR_COLOR_CURRENT_X 0x0003u
#define HA_ZB_ATTR_COLOR_CURRENT_Y 0x0004u
#define HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE 0x0007u
#define HA_ZB_ATTR_COLOR_COLOR_MODE 0x0008u

#define HA_ZB_ATTR_IAS_ZONE_ZONE_STATE 0x0000u
#define HA_ZB_ATTR_IAS_ZONE_ZONE_TYPE 0x0001u
#define HA_ZB_ATTR_IAS_ZONE_ZONE_STATUS 0x0002u

/* Типы данных ZCL (ZCL data type id) — чем интерпретировать значение атрибута. */
#define HA_ZB_TYPE_NONE 0x00u
#define HA_ZB_TYPE_BOOL 0x10u
#define HA_ZB_TYPE_BITMAP8 0x18u
#define HA_ZB_TYPE_UINT8 0x20u
#define HA_ZB_TYPE_UINT16 0x21u
#define HA_ZB_TYPE_UINT32 0x23u
#define HA_ZB_TYPE_INT8 0x28u
#define HA_ZB_TYPE_INT16 0x29u
#define HA_ZB_TYPE_INT32 0x2Bu
#define HA_ZB_TYPE_ENUM8 0x30u
#define HA_ZB_TYPE_ENUM16 0x31u
#define HA_ZB_TYPE_SINGLE_FLOAT 0x39u
#define HA_ZB_TYPE_OCTET_STRING 0x41u
#define HA_ZB_TYPE_CHAR_STRING 0x42u

/* Роль кластера в Simple Descriptor: чем определять «кнопка» vs «реле/лампа». */
#define HA_ZB_ROLE_CLIENT 0u
#define HA_ZB_ROLE_SERVER 1u

#ifdef __cplusplus
}
#endif
