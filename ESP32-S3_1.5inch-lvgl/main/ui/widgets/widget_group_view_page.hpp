#pragma once

#include <stddef.h>
#include <stdint.h>

#include "widget_endpoint_card.hpp"
#include "widget_group_state.hpp"
#include "widget_page.hpp"

class WidgetGroupViewPage : public WidgetPage {
public:
    explicit WidgetGroupViewPage(lv_obj_t *parent);
    ~WidgetGroupViewPage() override;

    lv_obj_t *root() const override;
    void render() override;
    void render_if_needed() override;

    void set_selection(size_t group_index, size_t item_index);
    size_t group_index() const;
    size_t item_index() const;

private:
    void update_header(const WidgetGroupState &state);
    void update_card(const WidgetGroupState &state);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *subtitle_ = nullptr;
    WidgetEndpointCard *card_ = nullptr;
    size_t group_index_ = 0;
    size_t item_index_ = 0;
    uint32_t last_group_version_ = 0;
    uint32_t last_item_version_ = 0;
};
