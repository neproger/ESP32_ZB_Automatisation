#pragma once

#include <stddef.h>

#include "widget_base.hpp"
#include "widget_group_view_page.hpp"

class WidgetUiRoot : public WidgetBase {
public:
    explicit WidgetUiRoot(lv_obj_t *parent);
    ~WidgetUiRoot() override;

    lv_obj_t *root() const override;
    void render() override;
    void render_if_needed() override;

    void transition_to_selection(size_t group_index,
                                 size_t item_index,
                                 WidgetGroupViewPage::Transition transition);

    size_t group_index() const;
    size_t item_index() const;

private:
    static lv_screen_load_anim_t map_transition(WidgetGroupViewPage::Transition transition);
    WidgetGroupViewPage *build_page(size_t group_index, size_t item_index);

    WidgetGroupViewPage *group_page_ = nullptr;
    size_t group_index_ = 0;
    size_t item_index_ = 0;
};
