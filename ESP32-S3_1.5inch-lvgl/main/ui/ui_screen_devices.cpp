#include "ui_screen_devices.hpp"

#include <atomic>

#include "lvgl.h"
#include "widgets/widget_group_state.hpp"
#include "widgets/widget_ui_root.hpp"

namespace
{
constexpr uint32_t kModelPollPeriodMs = 80;

WidgetUiRoot *s_ui_root = nullptr;
std::atomic<int> s_pending_group_delta{0};
std::atomic<int> s_pending_item_delta{0};
size_t s_target_group_index = 0;
size_t s_target_item_index = 0;
uint32_t s_last_model_poll_ms = 0;

size_t wrap_index(size_t base, int delta, size_t count)
{
    if (count == 0) {
        return 0;
    }
    const int64_t count64 = (int64_t)count;
    int64_t next = ((int64_t)base + (int64_t)delta) % count64;
    if (next < 0) {
        next += count64;
    }
    return (size_t)next;
}

WidgetGroupState read_state(size_t group_index, size_t item_index)
{
    WidgetGroupState state = {};
    (void)widget_group_state_read(group_index, item_index, &state);
    return state;
}

void sync_current_to_target(WidgetGroupViewPage::Transition transition)
{
    if (!s_ui_root) {
        return;
    }
    if (s_ui_root->group_index() == s_target_group_index &&
        s_ui_root->item_index() == s_target_item_index) {
        return;
    }
    s_ui_root->transition_to_selection(s_target_group_index, s_target_item_index, transition);
}

void apply_pending_targets(void)
{
    if (!s_ui_root) {
        return;
    }

    WidgetGroupViewPage::Transition transition = WidgetGroupViewPage::Transition::None;
    const int group_delta = s_pending_group_delta.exchange(0, std::memory_order_acq_rel);
    if (group_delta != 0) {
        WidgetGroupState state = read_state(s_target_group_index, s_target_item_index);
        if (state.group_count > 0) {
            s_target_group_index = wrap_index(s_target_group_index, group_delta, state.group_count);
            s_target_item_index = 0;
            transition = (group_delta > 0)
                ? WidgetGroupViewPage::Transition::SlideLeft
                : WidgetGroupViewPage::Transition::SlideRight;
        }
    } else {
        const int item_delta = s_pending_item_delta.exchange(0, std::memory_order_acq_rel);
        if (item_delta != 0) {
            WidgetGroupState state = read_state(s_target_group_index, s_target_item_index);
            if (state.item_count > 0) {
                s_target_item_index = wrap_index(s_target_item_index, item_delta, state.item_count);
                transition = (item_delta > 0)
                    ? WidgetGroupViewPage::Transition::SlideUp
                    : WidgetGroupViewPage::Transition::SlideDown;
            }
        }
    }

    sync_current_to_target(transition);
}
} // namespace

void ui_screen_devices_init(lv_obj_t *root)
{
    delete s_ui_root;
    s_ui_root = new WidgetUiRoot(root);
    s_target_group_index = 0;
    s_target_item_index = 0;
    s_last_model_poll_ms = 0;
    s_pending_group_delta.store(0, std::memory_order_release);
    s_pending_item_delta.store(0, std::memory_order_release);
}

void ui_screen_devices_render_if_needed(void)
{
    if (!s_ui_root) {
        return;
    }
    apply_pending_targets();

    const uint32_t now_ms = lv_tick_get();
    if (s_last_model_poll_ms != 0 && (now_ms - s_last_model_poll_ms) < kModelPollPeriodMs) {
        return;
    }
    s_last_model_poll_ms = now_ms;
    s_ui_root->render_if_needed();
}

void ui_screen_devices_step_group(int delta)
{
    if (delta == 0) {
        return;
    }
    s_pending_group_delta.fetch_add(delta, std::memory_order_acq_rel);
}

void ui_screen_devices_step_item(int delta)
{
    if (delta == 0) {
        return;
    }
    s_pending_item_delta.fetch_add(delta, std::memory_order_acq_rel);
}

void ui_screen_devices_clear_pending_navigation(void)
{
    s_pending_group_delta.store(0, std::memory_order_release);
    s_pending_item_delta.store(0, std::memory_order_release);
}
