#pragma once

#include <string.h>

#include "gw_proto/gw_proto_types.h"

struct WidgetEndpointRef {
    gw_device_uid_t uid = {};
    uint8_t endpoint = 0;

    bool valid() const
    {
        return uid.uid[0] != '\0' && endpoint != 0;
    }
};

inline bool widget_endpoint_ref_equals(const WidgetEndpointRef &lhs, const WidgetEndpointRef &rhs)
{
    return lhs.endpoint == rhs.endpoint &&
           strncmp(lhs.uid.uid, rhs.uid.uid, sizeof(lhs.uid.uid)) == 0;
}
