#include "widget_ui_root.hpp"

WidgetUiRoot::WidgetUiRoot(lv_obj_t *parent)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    group_page_ = new WidgetGroupViewPage(root_);
    render();
}

WidgetUiRoot::~WidgetUiRoot()
{
    delete group_page_;
    group_page_ = nullptr;
    if (root_) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
}

lv_obj_t *WidgetUiRoot::root() const
{
    return root_;
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

void WidgetUiRoot::set_group_index(size_t group_index)
{
    if (group_index_ == group_index) {
        return;
    }
    group_index_ = group_index;
    item_index_ = 0;
    render();
}

void WidgetUiRoot::set_item_index(size_t item_index)
{
    if (item_index_ == item_index) {
        return;
    }
    item_index_ = item_index;
    render();
}

void WidgetUiRoot::set_selection(size_t group_index, size_t item_index)
{
    if (group_index_ == group_index && item_index_ == item_index) {
        return;
    }
    group_index_ = group_index;
    item_index_ = item_index;
    render();
}

size_t WidgetUiRoot::group_index() const
{
    return group_index_;
}

size_t WidgetUiRoot::item_index() const
{
    return item_index_;
}
