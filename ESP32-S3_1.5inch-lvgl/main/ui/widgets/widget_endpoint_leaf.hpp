#pragma once

#include "widget_base.hpp"
#include "widget_endpoint_ref.hpp"

class WidgetEndpointLeaf : public WidgetBase {
public:
    explicit WidgetEndpointLeaf(const WidgetEndpointRef &ref)
        : ref_(ref)
    {
    }

    ~WidgetEndpointLeaf() override = default;

    void set_ref(const WidgetEndpointRef &ref)
    {
        ref_ = ref;
        last_version_ = 0;
    }

protected:
    WidgetEndpointRef ref_ = {};
    uint32_t last_version_ = 0;
};
