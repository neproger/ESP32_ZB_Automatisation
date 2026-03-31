#include "widget_ui_root.hpp"

#include "lvgl.h"
#include "ui_app.hpp"

WidgetUiRoot::WidgetUiRoot(lv_obj_t *parent)
{
    (void)parent;
    group_page_ = build_page(group_index_, item_index_);
    if (group_page_) {
        lv_screen_load(group_page_->root());
    }
}

WidgetUiRoot::~WidgetUiRoot()
{
    if (group_page_ && group_page_->root() && lv_obj_is_valid(group_page_->root())) {
        lv_obj_delete(group_page_->root());
    }
    group_page_ = nullptr;
}

lv_obj_t *WidgetUiRoot::root() const
{
    return group_page_ ? group_page_->root() : nullptr;
}

void WidgetUiRoot::render()
{
    if (group_page_) {
        group_page_->set_selection(group_index_, item_index_);
    }
}

void WidgetUiRoot::render_if_needed()
{
    if (group_page_) {
        group_page_->render_if_needed();
    }
}

void WidgetUiRoot::transition_to_selection(size_t group_index,
                                           size_t item_index,
                                           WidgetGroupViewPage::Transition transition)
{
    if (group_index_ == group_index && item_index_ == item_index) {
        return;
    }

    lv_display_t *display = lv_display_get_default();
    if (display && lv_display_get_screen_loading(display) != nullptr) {
        return;
    }

    WidgetGroupViewPage *next_page = build_page(group_index, item_index);
    if (!next_page) {
        return;
    }

    group_page_ = next_page;
    group_index_ = group_index;
    item_index_ = item_index;

    lv_screen_load_anim(group_page_->root(), map_transition(transition), 220, 0, true);
}

size_t WidgetUiRoot::group_index() const
{
    return group_index_;
}

size_t WidgetUiRoot::item_index() const
{
    return item_index_;
}

lv_screen_load_anim_t WidgetUiRoot::map_transition(WidgetGroupViewPage::Transition transition)
{
    switch (transition) {
    case WidgetGroupViewPage::Transition::SlideLeft:
        return LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
    case WidgetGroupViewPage::Transition::SlideRight:
        return LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
    case WidgetGroupViewPage::Transition::SlideUp:
        return LV_SCREEN_LOAD_ANIM_MOVE_TOP;
    case WidgetGroupViewPage::Transition::SlideDown:
        return LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM;
    case WidgetGroupViewPage::Transition::None:
    default:
        return LV_SCREEN_LOAD_ANIM_NONE;
    }
}

WidgetGroupViewPage *WidgetUiRoot::build_page(size_t group_index, size_t item_index)
{
    WidgetGroupViewPage *page = new WidgetGroupViewPage(nullptr);
    if (!page) {
        return nullptr;
    }
    page->set_selection(group_index, item_index);
    ui_app_attach_screen_input(page->root());
    return page;
}
