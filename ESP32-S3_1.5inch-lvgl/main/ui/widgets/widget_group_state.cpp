#include "widget_group_state.hpp"

#include <string.h>

#include "esp_attr.h"

namespace
{
static constexpr size_t kMaxGroups = 24;
static constexpr size_t kMaxItems = 256;
EXT_RAM_BSS_ATTR static gw_group_entry_t s_groups_snapshot[kMaxGroups];
EXT_RAM_BSS_ATTR static gw_group_item_t s_items_snapshot[kMaxItems];

void copy_group_item(const gw_group_item_t &src, WidgetGroupItemState *dst)
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

    const size_t group_count = gw_group_store_list(s_groups_snapshot, kMaxGroups);
    out_state->group_count = group_count;

    if (group_count == 0 || group_index >= group_count) {
        return false;
    }

    out_state->has_group = true;
    out_state->group = s_groups_snapshot[group_index];
    out_state->active_group_index = group_index;

    const size_t total_item_count = gw_group_store_list_items(s_items_snapshot, kMaxItems);

    size_t matching_count = 0;
    size_t selected_match_index = 0;
    bool selected_found = false;

    for (size_t i = 0; i < total_item_count; ++i) {
        if (strncmp(s_items_snapshot[i].group_id, out_state->group.id, sizeof(s_items_snapshot[i].group_id)) != 0) {
            continue;
        }

        if (!selected_found && matching_count == item_index) {
            copy_group_item(s_items_snapshot[i], &out_state->active_item);
            selected_match_index = matching_count;
            selected_found = true;
        }
        ++matching_count;
    }

    out_state->item_count = matching_count;
    if (matching_count == 0) {
        out_state->active_item_index = 0;
        out_state->has_active_item = false;
        return true;
    }

    if (!selected_found) {
        for (size_t i = 0; i < total_item_count; ++i) {
            if (strncmp(s_items_snapshot[i].group_id, out_state->group.id, sizeof(s_items_snapshot[i].group_id)) != 0) {
                continue;
            }
            copy_group_item(s_items_snapshot[i], &out_state->active_item);
            break;
        }
        out_state->active_item_index = 0;
    } else {
        out_state->active_item_index = selected_match_index;
    }

    out_state->has_active_item = out_state->active_item.ref.valid();
    return true;
}
