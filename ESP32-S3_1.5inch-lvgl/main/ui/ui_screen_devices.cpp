#include "ui_screen_devices.hpp"

#include "widgets/widget_group_state.hpp"
#include "widgets/widget_ui_root.hpp"

namespace
{
WidgetUiRoot *s_ui_root = nullptr;

WidgetGroupState read_current_state()
{
    WidgetGroupState state = {};
    if (s_ui_root) {
        (void)widget_group_state_read(s_ui_root->group_index(), s_ui_root->item_index(), &state);
    }
    return state;
}
} // namespace

void ui_screen_devices_init(lv_obj_t *root)
{
    delete s_ui_root;
    s_ui_root = new WidgetUiRoot(root);
}

void ui_screen_devices_render_if_needed(void)
{
    if (!s_ui_root) {
        return;
    }
    s_ui_root->render_if_needed();
}

bool ui_screen_devices_next_group(void)
{
    if (!s_ui_root) {
        return false;
    }

    WidgetGroupState state = read_current_state();
    if (state.group_count <= 1) {
        return false;
    }

    const size_t next_group = (s_ui_root->group_index() + 1) % state.group_count;
    s_ui_root->transition_to_selection(next_group, 0, WidgetGroupViewPage::Transition::SlideLeft);
    return true;
}

bool ui_screen_devices_prev_group(void)
{
    if (!s_ui_root) {
        return false;
    }

    WidgetGroupState state = read_current_state();
    if (state.group_count <= 1) {
        return false;
    }

    const size_t current = s_ui_root->group_index();
    const size_t prev_group = (current == 0) ? (state.group_count - 1) : (current - 1);
    s_ui_root->transition_to_selection(prev_group, 0, WidgetGroupViewPage::Transition::SlideRight);
    return true;
}

bool ui_screen_devices_next_item(void)
{
    if (!s_ui_root) {
        return false;
    }

    WidgetGroupState state = read_current_state();
    if (state.item_count <= 1) {
        return false;
    }

    const size_t next_item = (s_ui_root->item_index() + 1) % state.item_count;
    s_ui_root->transition_to_selection(s_ui_root->group_index(), next_item, WidgetGroupViewPage::Transition::SlideUp);
    return true;
}

bool ui_screen_devices_prev_item(void)
{
    if (!s_ui_root) {
        return false;
    }

    WidgetGroupState state = read_current_state();
    if (state.item_count <= 1) {
        return false;
    }

    const size_t current = s_ui_root->item_index();
    const size_t prev_item = (current == 0) ? (state.item_count - 1) : (current - 1);
    s_ui_root->transition_to_selection(s_ui_root->group_index(), prev_item, WidgetGroupViewPage::Transition::SlideDown);
    return true;
}
