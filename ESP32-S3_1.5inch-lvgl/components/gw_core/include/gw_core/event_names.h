#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Raw/runtime ingress events.
#define GW_EVT_ZIGBEE_ATTR_REPORT   "zigbee.attr_report"
#define GW_EVT_ZIGBEE_ATTR_READ     "zigbee.attr_read"
#define GW_EVT_ZIGBEE_READ_ATTR     "zigbee.read_attr"
#define GW_EVT_ZIGBEE_READ_ATTR_RSP "zigbee.read_attr_resp"
#define GW_EVT_ZIGBEE_COMMAND       "zigbee.command"
#define GW_EVT_ZIGBEE_DEVICE_JOIN   "zigbee.device_join"
#define GW_EVT_ZIGBEE_DEVICE_LEAVE  "zigbee.device_leave"
#define GW_EVT_ZIGBEE_NET_STATE     "zigbee.net_state"

// Canonical model events.
#define GW_EVT_DEVICE_STATE         "device.state"
#define GW_EVT_DEVICE_SYNC_READY    "device.sync_ready"
#define GW_EVT_DEVICE_REMOVE        "device.remove"
#define GW_EVT_GROUP_CHANGED        "group.changed"
#define GW_EVT_SETTINGS_CHANGED     "settings.changed"
#define GW_EVT_AUTOMATION_CHANGED   "automation.changed"

// Legacy/rules/debug events still in use.
#define GW_EVT_RULES_FIRED          "rules.fired"
#define GW_EVT_RULES_ACTION         "rules.action"

#ifdef __cplusplus
}
#endif
