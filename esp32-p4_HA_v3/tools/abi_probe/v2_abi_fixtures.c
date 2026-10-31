#include <stdint.h>

#include "ha_model/ha_groups.h"
#include "ha_model/ha_rule.h"

#define FIXTURE(name) __attribute__((section(".abi_" #name), used))

FIXTURE(rule)
const ha_rule_t v2_rule_fixture = {
    .triggers = {
        {
            .entity = 0x1122334455667788ull,
            .event_id = HA_EVENT_SINGLE_PRESS,
            .property = 0,
            .value = 0.0f,
            .value2 = 0.0f,
            .minutes_of_day = 0,
            .kind = HA_TRIGGER_DEVICE_EVENT,
            .op = 0,
            .edge = 0,
            .weekday_mask = 0,
            .reserved = {0, 0},
        },
        {
            .entity = 0x8877665544332211ull,
            .event_id = 0,
            .property = HA_PROPERTY_TEMPERATURE,
            .value = 21.5f,
            .value2 = 25.0f,
            .minutes_of_day = 0,
            .kind = HA_TRIGGER_STATE,
            .op = HA_CONDITION_OP_BETWEEN,
            .edge = HA_TRIGGER_EDGE_RISING,
            .weekday_mask = 0,
            .reserved = {0, 0},
        },
        {
            .entity = 0,
            .event_id = 0,
            .property = 0,
            .value = 0.0f,
            .value2 = 0.0f,
            .minutes_of_day = 450,
            .kind = HA_TRIGGER_TIME,
            .op = 0,
            .edge = 0,
            .weekday_mask = 0x1F,
            .reserved = {0, 0},
        },
        {
            .entity = 0x1122334455667788ull,
            .event_id = 0,
            .property = HA_PROPERTY_POWER,
            .value = 1.0f,
            .value2 = 0.0f,
            .minutes_of_day = 0,
            .kind = HA_TRIGGER_STATE,
            .op = HA_CONDITION_OP_EQ,
            .edge = HA_TRIGGER_EDGE_FALLING,
            .weekday_mask = 0,
            .reserved = {0, 0},
        },
    },
    .actions = {
        {
            .ref = {
                .entity = 0x1122334455667788ull,
                .property = HA_PROPERTY_BRIGHTNESS,
                .reserved = 0,
            },
            .value = {
                .kind = HA_COMMAND_VALUE_NONE,
                .value = {0},
            },
            .action = HA_ACTION_SET,
            .reserved = {0, 0, 0},
        },
        {
            .ref = {
                .entity = 0x8877665544332211ull,
                .property = HA_PROPERTY_POWER,
                .reserved = 0,
            },
            .value = {
                .kind = HA_COMMAND_VALUE_SCALAR,
                .value = {
                    .scalar = {
                        .kind = HA_VALUE_U32,
                        .value = {.u32 = 75},
                    },
                },
            },
            .action = HA_ACTION_SET,
            .reserved = {0, 0, 0},
        },
        {
            .ref = {
                .entity = HA_ENTITY_ID_SOURCE,
                .property = HA_PROPERTY_COLOR,
                .reserved = 0,
            },
            .value = {
                .kind = HA_COMMAND_VALUE_XY,
                .value = {
                    .xy = {.x = 0.25f, .y = 0.5f},
                },
            },
            .action = HA_ACTION_SET,
            .reserved = {0, 0, 0},
        },
        {
            .ref = {
                .entity = 0x1122334455667788ull,
                .property = HA_PROPERTY_POWER,
                .reserved = 0,
            },
            .value = {
                .kind = HA_COMMAND_VALUE_NONE,
                .value = {0},
            },
            .action = HA_ACTION_TOGGLE,
            .reserved = {0, 0, 0},
        },
    },
    .conditions = {
        {
            .ref = {
                .entity = 0x1122334455667788ull,
                .property = HA_PROPERTY_TEMPERATURE,
                .reserved = 0,
            },
            .cmp = {
                .op = HA_CONDITION_OP_BETWEEN,
                .reserved = {0, 0, 0},
                .value = 18.0f,
                .value2 = 24.0f,
            },
            .reserved = 0,
        },
        {
            .ref = {
                .entity = HA_ENTITY_ID_SOURCE,
                .property = HA_PROPERTY_SYSTEM_HOUR,
                .reserved = 0,
            },
            .cmp = {
                .op = HA_CONDITION_OP_GE,
                .reserved = {0, 0, 0},
                .value = 7.0f,
                .value2 = 0.0f,
            },
            .reserved = 0,
        },
        {
            .ref = {
                .entity = 0x8877665544332211ull,
                .property = HA_PROPERTY_HUMIDITY,
                .reserved = 0,
            },
            .cmp = {
                .op = HA_CONDITION_OP_LE,
                .reserved = {0, 0, 0},
                .value = 60.0f,
                .value2 = 0.0f,
            },
            .reserved = 0,
        },
        {
            .ref = {
                .entity = 0x1122334455667788ull,
                .property = HA_PROPERTY_POWER,
                .reserved = 0,
            },
            .cmp = {
                .op = HA_CONDITION_OP_EQ,
                .reserved = {0, 0, 0},
                .value = 1.0f,
                .value2 = 0.0f,
            },
            .reserved = 0,
        },
    },
    .enabled = 1,
    .trigger_count = 4,
    .action_count = 4,
    .conditions_count = 4,
    .reserved = {0, 0, 0, 0},
    .name = "Canonical rule",
};

FIXTURE(group_key)
const ha_group_key_t v2_group_key_fixture = {
    .id = 0x0102030405060708ull,
};

FIXTURE(group_record)
const ha_group_record_t v2_group_record_fixture = {
    .title = "Canonical group",
};

FIXTURE(group_item_key)
const ha_group_item_key_t v2_group_item_key_fixture = {
    .group_id = 0x0102030405060708ull,
    .entity = 0x1122334455667788ull,
};

FIXTURE(group_item_record)
const ha_group_item_record_t v2_group_item_record_fixture = {
    .order = 7,
    .reserved = {0, 0},
    .title = "Canonical item",
};

FIXTURE(group_automation_key)
const ha_group_automation_key_t v2_group_automation_key_fixture = {
    .group_id = 0x0102030405060708ull,
    .automation_id = 0x1112131415161718ull,
};

FIXTURE(automation_key)
const ha_automation_key_t v2_automation_key_fixture = {
    .id = 0x1112131415161718ull,
};
