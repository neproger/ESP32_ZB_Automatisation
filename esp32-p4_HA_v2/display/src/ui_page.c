#include "ui_page.h"

#include <string.h>

#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_state.h"
#include "ui_commands.h"
#include "ui_compat.h"
#include "ui_style.h"
#include "ui_widgets.h"

#define UI_HEADER_H 72
#define UI_LIST_W 456
#define UI_LIST_H 616

/* Плитка = Entity (виджеты по capability) или правило (подпись + переключатель). */
typedef struct {
    ui_widget_t *widgets[HA_ENTITY_CAPABILITIES_MAX];
    ha_state_key_t states[HA_ENTITY_CAPABILITIES_MAX];
    size_t count;
    uint64_t automation;    /* AUTOMATION: id правила (0 — плитка Entity) */
    lv_obj_t *auto_title;   /* AUTOMATION: label подписи */
    lv_obj_t *auto_switch;  /* AUTOMATION: переключатель enabled */
    bool updating;          /* AUTOMATION: применяем состояние сами — не шлём команду */
} ui_page_entry_t;

struct ui_page {
    domain_t *domain;
    lv_obj_t *root;
    ui_page_entry_t *entries;
    size_t count;
    ui_page_nav_cb nav;
    void *nav_ctx;
};

/* Жест начался на управляющем элементе (слайдер/свич) — экран не листаем. */
static bool gesture_started_on_control(void)
{
    for (lv_obj_t *obj = lv_indev_get_active_obj(); obj != NULL; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_switch_class)) {
            return true;
        }
    }
    return false;
}

static void on_gesture(lv_event_t *event)
{
    ui_page_t *page = (ui_page_t *)lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (page == NULL || page->nav == NULL || indev == NULL) {
        return;
    }
    /* Навигация между группами: только горизонтальные жесты. Вертикаль отдана
     * скроллу списка. При drag по слайдеру/свичу не листаем — иначе значение
     * «переключает экран» на быстром движении. */
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if ((dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) && !gesture_started_on_control()) {
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

/* Есть ли свойство в capability (для свёртки составного виджета цвета). */
static bool has_property(const ha_entity_capabilities_record_t *caps, ha_property_id_t property)
{
    for (uint8_t i = 0; i < caps->property_count; i++) {
        if (caps->properties[i].property == property) {
            return true;
        }
    }
    return false;
}

/* Свойства Entity — из её capability. Порядок как в записи (form задаёт потребитель). */
static size_t entity_properties(domain_t *domain, ha_entity_id_t entity, ha_property_id_t *out,
                                size_t max)
{
    const ha_entity_key_t key = {.id = entity};
    ha_entity_capabilities_record_t caps = {0};
    if (sys_failed(domain_entity_get(domain, (domain_entity_t)HA_ENTITY_CAPABILITIES, &key,
                                     &caps))) {
        return 0;
    }
    /* Виджет цвета — составной (свотч + оттенок + насыщенность + яркость). Отдельные
     * BRIGHTNESS/COLOR_TEMPERATURE не строим: иначе яркость дублируется, а слайдер
     * температуры часто не поддержан устройством и «залипает». */
    const bool has_color = has_property(&caps, HA_PROPERTY_COLOR);
    size_t n = 0;
    for (uint8_t i = 0; i < caps.property_count && n < max; i++) {
        const ha_property_id_t property = caps.properties[i].property;
        if (has_color && (property == HA_PROPERTY_BRIGHTNESS ||
                          property == HA_PROPERTY_COLOR_TEMPERATURE)) {
            continue;
        }
        out[n++] = property;
    }
    return n;
}

static void on_automation_changed(lv_event_t *event)
{
    ui_page_entry_t *entry = (ui_page_entry_t *)lv_event_get_user_data(event);
    if (entry == NULL || entry->updating || entry->auto_switch == NULL) {
        return;
    }
    const bool on = lv_obj_has_state(entry->auto_switch, LV_STATE_CHECKED);
    (void)display_set_automation_enabled(entry->automation, on);
}

/* Карточка плитки: общая рамка + подпись. Возвращает карточку, *out_title — label. */
static lv_obj_t *make_card(lv_obj_t *list, const ui_page_item_t *item, lv_obj_t **out_title)
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
    if (out_title != NULL) {
        *out_title = title;
    }
    return card;
}

static lv_obj_t *make_item_card(ui_page_t *page, lv_obj_t *list, const ui_page_item_t *item,
                                ui_page_entry_t *entry)
{
    lv_obj_t *title = NULL;
    lv_obj_t *card = make_card(list, item, &title);

    if (item->kind == UI_PAGE_ITEM_AUTOMATION) {
        entry->automation = item->automation;
        entry->auto_title = title;
        entry->auto_switch = lv_switch_create(card);
        lv_obj_set_size(entry->auto_switch, 140, 70);
        if (item->enabled) {
            lv_obj_add_state(entry->auto_switch, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(entry->auto_switch, on_automation_changed, LV_EVENT_VALUE_CHANGED,
                            entry);
        return card;
    }

    ha_property_id_t props[HA_ENTITY_CAPABILITIES_MAX];
    const size_t n = entity_properties(page->domain, item->entity, props,
                                       HA_ENTITY_CAPABILITIES_MAX);
    entry->count = n;
    for (size_t i = 0; i < n; i++) {
        entry->states[i] = (ha_state_key_t){.entity = item->entity, .property = props[i]};
        entry->widgets[i] = ui_widget_create(page->domain, card, &entry->states[i]);
    }
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
        /* Плитки Entity заполняют не все поля, автоматизации — только auto_*: без
         * обнуления ui_page_apply читает мусор в count/widgets. */
        memset(page->entries, 0, item_count * sizeof(*page->entries));
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
        make_item_card(page, list, &items[i], &page->entries[i]);
    }
    return page;
}

void ui_page_apply(ui_page_t *page)
{
    if (page == NULL) {
        return;
    }
    for (size_t i = 0; i < page->count; ++i) {
        ui_page_entry_t *entry = &page->entries[i];
        for (size_t w = 0; w < entry->count; ++w) {
            ha_state_record_t record = {0};
            const bool present = sys_ok(domain_entity_get(
                page->domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &entry->states[w],
                &record));
            ui_widget_apply(entry->widgets[w], &record.value, present);
        }
    }
}

void ui_page_apply_key(ui_page_t *page, const ha_state_key_t *key, const ha_value_t *value,
                       bool present)
{
    if (page == NULL || key == NULL) {
        return;
    }
    for (size_t i = 0; i < page->count; ++i) {
        ui_page_entry_t *entry = &page->entries[i];
        for (size_t w = 0; w < entry->count; ++w) {
            const ha_state_key_t *state = &entry->states[w];
            if (state->entity != key->entity) {
                continue;
            }
            const bool color = (ui_widget_kind_for(state->property) == UI_WIDGET_COLOR);
            const bool matches = (state->property == key->property) ||
                                 (color && (key->property == HA_PROPERTY_COLOR_X ||
                                            key->property == HA_PROPERTY_COLOR_Y ||
                                            key->property == HA_PROPERTY_BRIGHTNESS));
            if (matches) {
                ui_widget_apply(entry->widgets[w], value, present);
            }
        }
    }
}

void ui_page_apply_automation(ui_page_t *page, uint64_t automation, const char *name,
                              bool enabled)
{
    if (page == NULL) {
        return;
    }
    for (size_t i = 0; i < page->count; ++i) {
        ui_page_entry_t *entry = &page->entries[i];
        if (entry->automation != automation || entry->auto_switch == NULL) {
            continue;
        }
        if (name != NULL && name[0] != '\0' && entry->auto_title != NULL) {
            const char *current = lv_label_get_text(entry->auto_title);
            if (current == NULL || strcmp(current, name) != 0) {
                lv_label_set_text(entry->auto_title, name);
            }
        }
        entry->updating = true; /* не отправлять команду на собственное обновление */
        if (enabled) {
            lv_obj_add_state(entry->auto_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(entry->auto_switch, LV_STATE_CHECKED);
        }
        entry->updating = false;
    }
}

lv_obj_t *ui_page_root(const ui_page_t *page)
{
    return (page != NULL) ? page->root : NULL;
}
