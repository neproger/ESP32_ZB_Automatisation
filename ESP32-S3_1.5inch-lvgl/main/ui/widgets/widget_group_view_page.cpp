#include "widget_group_view_page.hpp"

#include <stdio.h>

#include "ui_style.hpp"

namespace
{
constexpr int32_t kSlideOffsetX = 28;
constexpr int32_t kSlideOffsetY = 22;
constexpr uint32_t kTransitionDurationMs = 180;

void anim_translate_x(void *var, int32_t value)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, value, 0);
}

void anim_translate_y(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

void anim_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

void align_card_below(lv_obj_t *root, lv_obj_t *subtitle, lv_obj_t *card)
{
    if (!root || !subtitle || !card) {
        return;
    }
    lv_obj_update_layout(root);
    const int32_t subtitle_bottom = lv_obj_get_y(subtitle) + lv_obj_get_height(subtitle);
    lv_obj_set_width(card, ui_style::kListWidth);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, subtitle_bottom + 6);
    lv_obj_update_layout(root);
    const int32_t root_h = lv_obj_get_height(root);
    const int32_t card_y = lv_obj_get_y(card);
    int32_t card_h = root_h - card_y - ui_style::kCardBottomOffset;
    if (card_h < 40) {
        card_h = 40;
    }
    lv_obj_set_height(card, card_h);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, subtitle_bottom + 6);
}
} // namespace

WidgetGroupViewPage::WidgetGroupViewPage(lv_obj_t *parent)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(root_);

    title_ = lv_label_create(root_);
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, ui_style::kTitleY);
    lv_obj_set_style_text_color(title_, lv_color_hex(ui_style::kTitleTextHex), 0);
    lv_obj_set_style_text_font(title_, ui_style::kFontTitle, 0);

    subtitle_ = lv_label_create(root_);
    lv_obj_align_to(subtitle_, title_, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_set_style_text_color(subtitle_, lv_color_hex(ui_style::kSubtitleTextHex), 0);
    lv_obj_set_style_text_font(subtitle_, ui_style::kFontSubtitle, 0);

    render();
}

WidgetGroupViewPage::~WidgetGroupViewPage()
{
    delete card_;
    card_ = nullptr;
}

lv_obj_t *WidgetGroupViewPage::root() const
{
    return root_;
}

void WidgetGroupViewPage::render()
{
    WidgetGroupState state = {};
    (void)widget_group_state_read(group_index_, item_index_, &state);
    update_header(state);
    update_card(state);
    last_group_count_ = state.group_count;
    last_item_count_ = state.item_count;
    last_has_group_ = state.has_group;
    last_has_active_item_ = state.has_active_item;
    last_group_version_ = state.group.version;
    last_item_version_ = state.has_active_item ? state.active_item.version : 0;
}

void WidgetGroupViewPage::render_if_needed()
{
    WidgetGroupState state = {};
    (void)widget_group_state_read(group_index_, item_index_, &state);

    const uint32_t group_version = state.has_group ? state.group.version : 0;
    const uint32_t item_version = state.has_active_item ? state.active_item.version : 0;
    const bool selection_changed =
        state.group_count != last_group_count_ ||
        state.item_count != last_item_count_ ||
        state.has_group != last_has_group_ ||
        state.has_active_item != last_has_active_item_ ||
        group_version != last_group_version_ ||
        item_version != last_item_version_;

    if (selection_changed) {
        update_header(state);
        update_card(state);
        last_group_count_ = state.group_count;
        last_item_count_ = state.item_count;
        last_has_group_ = state.has_group;
        last_has_active_item_ = state.has_active_item;
        last_group_version_ = group_version;
        last_item_version_ = item_version;
    } else if (card_) {
        card_->render_if_needed();
    }
}

void WidgetGroupViewPage::set_selection(size_t group_index, size_t item_index, Transition transition)
{
    if (group_index_ == group_index && item_index_ == item_index) {
        return;
    }
    group_index_ = group_index;
    item_index_ = item_index;
    last_group_count_ = 0;
    last_item_count_ = 0;
    last_has_group_ = false;
    last_has_active_item_ = false;
    last_group_version_ = 0;
    last_item_version_ = 0;
    render();
    play_transition(transition);
}

size_t WidgetGroupViewPage::group_index() const
{
    return group_index_;
}

size_t WidgetGroupViewPage::item_index() const
{
    return item_index_;
}

void WidgetGroupViewPage::play_transition(Transition transition)
{
    if (!root_ || transition == Transition::None) {
        return;
    }

    int32_t offset_x = 0;
    int32_t offset_y = 0;
    switch (transition) {
    case Transition::SlideLeft:
        offset_x = kSlideOffsetX;
        break;
    case Transition::SlideRight:
        offset_x = -kSlideOffsetX;
        break;
    case Transition::SlideUp:
        offset_y = kSlideOffsetY;
        break;
    case Transition::SlideDown:
        offset_y = -kSlideOffsetY;
        break;
    case Transition::None:
    default:
        return;
    }

    lv_obj_set_style_translate_x(root_, offset_x, 0);
    lv_obj_set_style_translate_y(root_, offset_y, 0);
    lv_obj_set_style_opa(root_, LV_OPA_70, 0);

    lv_anim_t anim;
    if (offset_x != 0) {
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, root_);
        lv_anim_set_values(&anim, offset_x, 0);
        lv_anim_set_duration(&anim, kTransitionDurationMs);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, anim_translate_x);
        lv_anim_start(&anim);
    }
    if (offset_y != 0) {
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, root_);
        lv_anim_set_values(&anim, offset_y, 0);
        lv_anim_set_duration(&anim, kTransitionDurationMs);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, anim_translate_y);
        lv_anim_start(&anim);
    }

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, root_);
    lv_anim_set_values(&anim, LV_OPA_70, LV_OPA_COVER);
    lv_anim_set_duration(&anim, kTransitionDurationMs - 20);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, anim_opa);
    lv_anim_start(&anim);
}

void WidgetGroupViewPage::update_header(const WidgetGroupState &state)
{
    if (!title_ || !subtitle_) {
        return;
    }

    if (!state.has_group) {
        lv_label_set_text(title_, "No groups");
        lv_label_set_text(subtitle_, "Create groups in Web UI");
        return;
    }

    lv_label_set_text(title_, state.group.name[0] ? state.group.name : state.group.id);

    if (!state.has_active_item || state.item_count == 0) {
        lv_label_set_text(subtitle_, "No endpoints in group");
        return;
    }

    const char *label = state.active_item.label[0] ? state.active_item.label : state.active_item.ref.uid.uid;
    lv_label_set_text_fmt(subtitle_,
                          "#%u/%u %s",
                          (unsigned)(state.active_item_index + 1),
                          (unsigned)state.item_count,
                          label);
}

void WidgetGroupViewPage::update_card(const WidgetGroupState &state)
{
    if (!root_ || !subtitle_) {
        return;
    }

    if (!state.has_active_item) {
        delete card_;
        card_ = nullptr;
        return;
    }

    if (!card_) {
        card_ = new WidgetEndpointCard(root_, state.active_item.ref);
    } else {
        card_->set_ref(state.active_item.ref);
    }

    if (card_) {
        align_card_below(root_, subtitle_, card_->root());
        card_->render_if_needed();
    }
}
