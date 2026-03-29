#pragma once

#include "ui_control_ack.hpp"
#include "widget_endpoint_leaf.hpp"

class WidgetOnOffControl : public WidgetEndpointLeaf {
public:
    WidgetOnOffControl(lv_obj_t *parent, const WidgetEndpointRef &ref);
    ~WidgetOnOffControl() override;

    lv_obj_t *root() const override;
    void render() override;
    void render_if_needed() override;

private:
    static void on_row_clicked(lv_event_t *event);
    void handle_click();
    void apply_state(bool has_value, bool value, uint32_t version);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *label_ = nullptr;
    lv_obj_t *switch_ = nullptr;
};

class WidgetLevelControl : public WidgetEndpointLeaf {
public:
    WidgetLevelControl(lv_obj_t *parent, const WidgetEndpointRef &ref);
    ~WidgetLevelControl() override;

    lv_obj_t *root() const override;
    void render() override;
    void render_if_needed() override;

private:
    static void on_slider_released(lv_event_t *event);
    void handle_release();

    lv_obj_t *root_ = nullptr;
    lv_obj_t *label_ = nullptr;
    lv_obj_t *slider_ = nullptr;
};

class WidgetColorControl : public WidgetEndpointLeaf {
public:
    WidgetColorControl(lv_obj_t *parent, const WidgetEndpointRef &ref);
    ~WidgetColorControl() override;

    lv_obj_t *root() const override;
    void render() override;
    void render_if_needed() override;

private:
    static void on_hs_released(lv_event_t *event);
    void handle_release();
    void apply_color(uint32_t x, uint32_t y, uint32_t version, bool update_sliders);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *label_hue_ = nullptr;
    lv_obj_t *slider_hue_ = nullptr;
    lv_obj_t *label_sat_ = nullptr;
    lv_obj_t *slider_sat_ = nullptr;
    uint16_t cached_hue_ = 0;
    uint8_t cached_sat_ = 0;
    bool has_cached_hs_ = false;
};

class WidgetValueLabel : public WidgetEndpointLeaf {
public:
    enum class Kind : uint8_t {
        Temperature = 1,
        Humidity = 2,
        Battery = 3,
    };

    WidgetValueLabel(lv_obj_t *parent, const WidgetEndpointRef &ref, Kind kind);
    ~WidgetValueLabel() override;

    lv_obj_t *root() const override;
    void render() override;
    void render_if_needed() override;

private:
    Kind kind_;
    lv_obj_t *label_ = nullptr;
};
