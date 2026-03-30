#pragma once

#include "ui_style.hpp"
#include "widget_base.hpp"
#include "widget_endpoint_controls.hpp"
#include "widget_endpoint_ref.hpp"
#include "widget_endpoint_state.hpp"

class WidgetEndpointCard : public WidgetBase {
public:
    WidgetEndpointCard(lv_obj_t *parent, const WidgetEndpointRef &ref);
    ~WidgetEndpointCard() override;

    lv_obj_t *root() const override;
    void render() override;
    void render_if_needed() override;

    void set_ref(const WidgetEndpointRef &ref);
    const WidgetEndpointRef &ref() const;

private:
    void update_title(const WidgetEndpointState &state);
    void update_subtitle(const WidgetEndpointState &state);
    void update_summary(const WidgetEndpointState &state);
    void rebuild_if_needed(const WidgetEndpointState &state);
    void refresh_children(const WidgetEndpointState &state);
    void clear_children();

    WidgetEndpointRef ref_ = {};
    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *subtitle_ = nullptr;
    lv_obj_t *summary_ = nullptr;
    WidgetOnOffControl *onoff_ = nullptr;
    WidgetLevelControl *level_ = nullptr;
    WidgetColorControl *color_ = nullptr;
    WidgetValueLabel *temperature_ = nullptr;
    WidgetValueLabel *humidity_ = nullptr;
    WidgetValueLabel *battery_ = nullptr;
    uint32_t last_topology_version_ = 0;
    uint32_t last_state_version_ = 0;
    uint32_t last_summary_version_ = 0;
};
