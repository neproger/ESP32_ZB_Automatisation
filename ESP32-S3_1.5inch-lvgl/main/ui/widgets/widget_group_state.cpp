#include "widget_group_state.hpp"

#include <string.h>

namespace
{
void copy_group_item(const gw_proto_group_item_v1_t &src, WidgetGroupItemState *dst)
{
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    dst->ref.uid = src.device_uid;
    dst->ref.endpoint = src.endpoint;
    dst->version = src.version;
    strlcpy(dst->label, src.label, sizeof(dst->label));
}
} // namespace

bool widget_group_state_read(size_t group_index, size_t item_index, WidgetGroupState *out_state)
{
    if (!out_state) {
        return false;
    }

    *out_state = WidgetGroupState{};

    const size_t group_count = gw_model_count_groups();
    out_state->group_count = group_count;

    if (group_count == 0 || group_index >= group_count) {
        return false;
    }

    gw_proto_group_v1_t group = {};
    if (gw_model_get_group_by_index(group_index, &group) != ESP_OK) {
        return false;
    }
    out_state->has_group = true;
    out_state->group = group;
    out_state->active_group_index = group_index;

    const size_t item_count = gw_model_count_group_items_for_group(out_state->group.id);
    out_state->item_count = item_count;
    if (item_count == 0) {
        out_state->active_item_index = 0;
        out_state->has_active_item = false;
        return true;
    }

    size_t resolved_item_index = item_index;
    if (resolved_item_index >= item_count) {
        resolved_item_index = 0;
    }

    gw_proto_group_item_v1_t item = {};
    if (gw_model_get_group_item_for_group_by_index(out_state->group.id, resolved_item_index, &item) != ESP_OK) {
        return false;
    }
    copy_group_item(item, &out_state->active_item);
    out_state->active_item_index = resolved_item_index;

    out_state->has_active_item = out_state->active_item.ref.valid();
    return true;
}
