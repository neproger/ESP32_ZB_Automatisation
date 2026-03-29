#include "widget_group_view_page.hpp"

#include <stdio.h>

#include "ui_style.hpp"

namespace
{
void align_card_below(lv_obj_t *root, lv_obj_t *subtitle, lv_obj_t *card)
{
    if (!root || !subtitle || !card) {
        return;
    }
    lv_obj_align_to(card, subtitle, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_update_layout(root);
    const int32_t root_h = lv_obj_get_height(root);
    const int32_t card_y = lv_obj_get_y(card);
    int32_t card_h = root_h - card_y - ui_style::kCardBottomOffset;
    if (card_h < 40) {
        card_h = 40;
    }
    lv_obj_set_height(card, card_h);
    lv_obj_set_width(card, ui_style::kListWidth);
}
} // namespace

WidgetGroupViewPage::WidgetGroupViewPage(lv_obj_t *parent)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

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
        group_version != last_group_version_ ||
        item_version != last_item_version_;

    if (selection_changed) {
        update_header(state);
        update_card(state);
        last_group_version_ = group_version;
        last_item_version_ = item_version;
    } else if (card_) {
        card_->render_if_needed();
    }
}

void WidgetGroupViewPage::set_selection(size_t group_index, size_t item_index)
{
    if (group_index_ == group_index && item_index_ == item_index) {
        return;
    }
    group_index_ = group_index;
    item_index_ = item_index;
    last_group_version_ = 0;
    last_item_version_ = 0;
    render();
}

size_t WidgetGroupViewPage::group_index() const
{
    return group_index_;
}

size_t WidgetGroupViewPage::item_index() const
{
    return item_index_;
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
