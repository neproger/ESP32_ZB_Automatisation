#include "ui_page.h"

#include "ha_model/ha_entities.h"
#include "ui_compat.h"
#include "ui_style.h"
#include "ui_widgets.h"

#define UI_HEADER_H 72
#define UI_LIST_W 456
#define UI_LIST_H 616

typedef struct {
    ui_widget_t *widget;
    ha_zb_state_key_t state;
} ui_page_entry_t;

struct ui_page {
    domain_t *domain;
    lv_obj_t *root;
    ui_page_entry_t *entries;
    size_t count;
    ui_page_nav_cb nav;
    void *nav_ctx;
};

static void on_gesture(lv_event_t *event)
{
    ui_page_t *page = (ui_page_t *)lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (page == NULL || page->nav == NULL || indev == NULL) {
        return;
    }
    /* Навигация между группами: только горизонтальные жесты. Вертикаль отдана
     * скроллу списка. */
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        page->nav(page->nav_ctx, dir);
    }
}

/* Экран удалён — освобождаем обёртку страницы и массив элементов. Сами виджеты
 * (их lv_obj — дети списка) удаляются вместе с экраном и освобождают свои обёртки. */
static void on_root_deleted(lv_event_t *event)
{
    ui_page_t *page = (ui_page_t *)lv_event_get_user_data(event);
    if (page != NULL) {
        lv_free(page->entries);
        lv_free(page);
    }
}

static lv_obj_t *make_item_card(domain_t *domain, lv_obj_t *list, const ui_page_item_t *item,
                                ui_page_entry_t *entry)
{
    lv_obj_t *card = lv_obj_create(list);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    ui_set_scrollable(card, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_MUTED), 0);
    lv_label_set_text(title, (item->title != NULL) ? item->title : "");

    entry->state = (item->state != NULL) ? *item->state : (ha_zb_state_key_t){0};
    entry->widget = ui_widget_create(domain, card, &entry->state);
    return card;
}

ui_page_t *ui_page_create(domain_t *domain, const char *group_title,
                          const ui_page_item_t *items, size_t item_count, ui_page_nav_cb nav,
                          void *nav_ctx)
{
    ui_page_t *page = lv_malloc(sizeof(*page));
    if (page == NULL) {
        return NULL;
    }
    *page = (ui_page_t){0};
    page->domain = domain;
    page->count = item_count;
    page->nav = nav;
    page->nav_ctx = nav_ctx;

    if (item_count > 0) {
        page->entries = lv_malloc(item_count * sizeof(*page->entries));
        if (page->entries == NULL) {
            lv_free(page);
            return NULL;
        }
    }

    page->root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(page->root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    ui_set_scrollable(page->root, false);
    lv_obj_add_event_cb(page->root, on_gesture, LV_EVENT_GESTURE, page);
    lv_obj_add_event_cb(page->root, on_root_deleted, LV_EVENT_DELETE, page);

    lv_obj_t *header = lv_obj_create(page->root);
    lv_obj_set_size(header, lv_pct(100), UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    ui_set_scrollable(header, false);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(title, (group_title != NULL) ? group_title : "");

    lv_obj_t *list = lv_obj_create(page->root);
    lv_obj_set_size(list, UI_LIST_W, UI_LIST_H);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + UI_HEADER_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 16, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < item_count; ++i) {
        make_item_card(domain, list, &items[i], &page->entries[i]);
    }
    return page;
}

void ui_page_apply(ui_page_t *page)
{
    if (page == NULL) {
        return;
    }
    for (size_t i = 0; i < page->count; ++i) {
        ha_zb_state_record_t record = {0};
        const bool present = sys_ok(domain_entity_get(page->domain,
                                                      (domain_entity_t)HA_ENTITY_STATE,
                                                      &page->entries[i].state, &record));
        ui_widget_apply(page->entries[i].widget, &record, present);
    }
}

lv_obj_t *ui_page_root(const ui_page_t *page)
{
    return (page != NULL) ? page->root : NULL;
}
