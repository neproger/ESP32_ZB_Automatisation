use super::*;

unsafe fn control_context(
    entity: EntityId,
    property: PropertyId,
    control: *mut lv_obj_t,
) -> *mut CmdCtx {
    Box::into_raw(Box::new(CmdCtx {
        entity,
        property,
        control,
        locked: AtomicBool::new(false),
        syncing: AtomicBool::new(false),
        has_last: AtomicBool::new(false),
        last_bits: AtomicU32::new(0),
        deadline: Mutex::new(None),
    }))
}

unsafe fn attach_control(
    control: *mut lv_obj_t,
    context: *mut CmdCtx,
    code: lv_event_code_t,
) {
    lv_obj_add_event_cb(control, Some(on_control), code, context as *mut _);
    lv_obj_add_event_cb(
        control,
        Some(free_context),
        lv_event_code_t_LV_EVENT_DELETE,
        context as *mut _,
    );
}

unsafe fn build_widget(
    domain: &DomainHandle,
    row: *mut lv_obj_t,
    entity: EntityId,
    property: PropertyId,
    widgets: &mut Vec<WidgetEntry>,
) {
    let kind = kind_for(property);
    let state = read_state(domain, entity, property);
    match kind {
        WidgetKind::Switch => {
            let control = switch_control(
                row,
                state.as_ref().and_then(Value::as_bool).unwrap_or(false),
            );
            let context = control_context(entity, property, control);
            attach_control(control, context, lv_event_code_t_LV_EVENT_VALUE_CHANGED);
            widgets.push(WidgetEntry {
                entity,
                property,
                kind,
                control,
                value_label: std::ptr::null_mut(),
                ctx: context,
            });
        }
        WidgetKind::Slider => {
            let (min, max) = if property == PROPERTY_COLOR_TEMPERATURE {
                (2000, 6500)
            } else {
                (0, 100)
            };
            let initial = state
                .as_ref()
                .and_then(numeric)
                .unwrap_or(min as f32)
                .round() as i32;
            let text = state
                .as_ref()
                .map(|value| format_value(property, value))
                .unwrap_or_else(|| "—".into());
            let (control, value_label) =
                slider_control(row, 170, min, max, initial, &text);
            let context = control_context(entity, property, control);
            attach_control(control, context, lv_event_code_t_LV_EVENT_RELEASED);
            widgets.push(WidgetEntry {
                entity,
                property,
                kind,
                control,
                value_label,
                ctx: context,
            });
        }
        WidgetKind::Value => {
            let text = state
                .as_ref()
                .map(|value| format_value(property, value))
                .unwrap_or_else(|| "—".into());
            let value_label = value_control(row, &text);
            widgets.push(WidgetEntry {
                entity,
                property,
                kind,
                control: value_label,
                value_label,
                ctx: std::ptr::null_mut(),
            });
        }
    }
}

unsafe fn build_color_widget(
    domain: &DomainHandle,
    card: *mut lv_obj_t,
    entity: EntityId,
    colors: &mut Vec<ColorEntry>,
) {
    let column = transparent_box(card, 400, 170);
    lv_obj_set_flex_flow(column, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 8, 0);
    lv_obj_set_flex_align(
        column,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );

    let swatch = lv_obj_create(column);
    lv_obj_set_size(swatch, 400, 44);
    lv_obj_set_style_radius(swatch, 12, 0);
    lv_obj_set_style_border_width(swatch, 2, 0);
    lv_obj_set_style_bg_opa(swatch, 255, 0);
    lv_obj_set_scrollable(swatch, false);

    let x = read_state(domain, entity, PROPERTY_COLOR_X)
        .as_ref()
        .and_then(numeric)
        .unwrap_or(0.25);
    let y = read_state(domain, entity, PROPERTY_COLOR_Y)
        .as_ref()
        .and_then(numeric)
        .unwrap_or(0.33);
    let (r, g, b) = xy_to_rgb8(x, y);
    lv_obj_set_style_bg_color(swatch, color(rgb_hex(r, g, b)), 0);
    let (hue, sat, _) = rgb8_to_hsv(r, g, b);

    let hue_row = widget_row(column, "Оттенок");
    let (hue_slider, hue_label) = slider_control(
        hue_row,
        190,
        0,
        359,
        hue.round() as i32,
        &format!("{}°", hue.round() as i32),
    );

    let sat_row = widget_row(column, "Насыщенность");
    let (sat_slider, sat_label) = slider_control(
        sat_row,
        160,
        0,
        100,
        (sat * 100.0).round() as i32,
        &format!("{} %", (sat * 100.0).round() as i32),
    );

    for slider in [hue_slider, sat_slider] {
        let context = Box::into_raw(Box::new(ColorCtx {
            entity,
            hue: hue_slider,
            sat: sat_slider,
            swatch,
            hue_label,
            sat_label,
        }));
        lv_obj_add_event_cb(
            slider,
            Some(on_color),
            lv_event_code_t_LV_EVENT_VALUE_CHANGED,
            context as *mut _,
        );
        lv_obj_add_event_cb(
            slider,
            Some(on_color),
            lv_event_code_t_LV_EVENT_RELEASED,
            context as *mut _,
        );
        lv_obj_add_event_cb(
            slider,
            Some(free_color_context),
            lv_event_code_t_LV_EVENT_DELETE,
            context as *mut _,
        );
    }

    colors.push(ColorEntry {
        entity,
        swatch,
        hue: hue_slider,
        sat: sat_slider,
        hue_label,
        sat_label,
        x,
        y,
    });
}

unsafe fn build_automation_card(
    parent: *mut lv_obj_t,
    item: &AutomationItem,
    automations: &mut Vec<AutoEntry>,
) {
    let (card, title_label) = widget_card(parent, &item.title, 138);
    let row = widget_row(card, "Активировать");
    let control = switch_control(row, item.enabled);

    let context = Box::into_raw(Box::new(AutoCtx {
        automation: item.automation,
    }));
    lv_obj_add_event_cb(
        control,
        Some(on_automation),
        lv_event_code_t_LV_EVENT_VALUE_CHANGED,
        context as *mut _,
    );
    lv_obj_add_event_cb(
        control,
        Some(free_auto_context),
        lv_event_code_t_LV_EVENT_DELETE,
        context as *mut _,
    );
    automations.push(AutoEntry {
        automation: item.automation,
        title: title_label,
        switch: control,
    });
}

unsafe fn build_card(
    domain: &DomainHandle,
    parent: *mut lv_obj_t,
    entity: EntityId,
    item_title: &str,
    widgets: &mut Vec<WidgetEntry>,
    colors: &mut Vec<ColorEntry>,
) {
    let properties = capabilities(domain, entity);
    let name = if item_title.is_empty() {
        entity_name(domain, entity)
    } else {
        item_title.to_string()
    };
    let mut rows_height = 0;
    for property in &properties {
        rows_height += if *property == PROPERTY_COLOR { 170 } else { 46 };
    }
    let spacing = properties.len() as i32 * 8;
    let (card, _) = widget_card(parent, &name, 84 + rows_height + spacing);

    for property in properties {
        if property == PROPERTY_COLOR {
            build_color_widget(domain, card, entity, colors);
            continue;
        }
        let row = widget_row(card, property_name(property));
        build_widget(domain, row, entity, property, widgets);
    }
}

pub(crate) unsafe fn build_page(domain: &DomainHandle, groups: &[GroupRef], index: usize) -> Page {
    let screen = lv_obj_create(std::ptr::null_mut());
    lv_obj_set_style_bg_color(screen, color(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, 255, 0);
    lv_obj_set_scrollable(screen, false);

    let mut widgets = Vec::new();
    let mut automations = Vec::new();
    let mut colors = Vec::new();
    let title = if groups.is_empty() {
        "Нет экранов"
    } else {
        groups[index].title.as_str()
    };
    let heading = label(screen, title, font_32(), COL_TEXT);
    lv_obj_align(heading, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 10);

    if !groups.is_empty() {
        let left = label(screen, ICON_LEFT, font_icons(), COL_TEXT);
        lv_obj_align(left, lv_align_t_LV_ALIGN_TOP_LEFT, 12, STATUSBAR_H + 12);
        nav_target(left, Nav::Delta(-1));

        let right = label(screen, ICON_RIGHT, font_icons(), COL_TEXT);
        lv_obj_align(right, lv_align_t_LV_ALIGN_TOP_RIGHT, -12, STATUSBAR_H + 12);
        nav_target(right, Nav::Delta(1));

        let list = lv_obj_create(screen);
        lv_obj_set_size(list, LIST_W, LIST_H);
        lv_obj_align(list, lv_align_t_LV_ALIGN_TOP_MID, 0, LIST_Y);
        lv_obj_set_style_bg_opa(list, 0, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_pad_all(list, 10, 0);
        lv_obj_set_style_pad_row(list, 12, 0);
        lv_obj_set_flex_flow(list, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(
            list,
            0,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_START,
        );

        for item in collect_page_items(domain, groups[index].id) {
            match item {
                PageItem::Entity(item) => build_card(
                    domain,
                    list,
                    item.entity,
                    &item.title,
                    &mut widgets,
                    &mut colors,
                ),
                PageItem::Automation(item) => {
                    build_automation_card(list, &item, &mut automations)
                }
            }
        }

        if groups.len() > 1 {
            let dots = transparent_box(screen, 0, 20);
            lv_obj_set_width(dots, (groups.len().min(24) as i32) * 22);
            lv_obj_align(dots, lv_align_t_LV_ALIGN_BOTTOM_MID, 0, -6);
            lv_obj_set_style_pad_column(dots, 10, 0);
            lv_obj_set_flex_flow(dots, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(
                dots,
                lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
                lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
                lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            );
            for i in 0..groups.len().min(24) {
                let dot = lv_obj_create(dots);
                lv_obj_set_size(dot, 12, 12);
                lv_obj_set_style_radius(dot, 6, 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_bg_color(
                    dot,
                    color(if i == index { COL_ACCENT } else { COL_CHIP }),
                    0,
                );
                lv_obj_set_style_bg_opa(dot, 255, 0);
                lv_obj_set_style_pad_all(dot, 0, 0);
                lv_obj_set_scrollable(dot, false);
                nav_target(dot, Nav::Index(i));
            }
        }
    }

    lv_screen_load(screen);
    Page {
        screen,
        widgets,
        colors,
        automations,
    }
}

pub(crate) unsafe fn replace_page(page: Page) {
    let old = PAGE.lock().unwrap().take();
    lv_screen_load(page.screen);
    if let Some(old) = old {
        lv_obj_delete(old.screen);
    }
    *PAGE.lock().unwrap() = Some(page);
}

unsafe extern "C" fn on_control(event: *mut lv_event_t) {
    let target = lv_event_get_target_obj(event);
    let context = lv_event_get_user_data(event) as *const CmdCtx;
    if target.is_null() || context.is_null() {
        return;
    }
    let context = &*context;
    log::debug!(
        "display: control entity={:016x} property={} syncing={} locked={}",
        context.entity,
        context.property,
        context.syncing.load(Ordering::Relaxed),
        context.locked.load(Ordering::Relaxed)
    );
    if context.syncing.load(Ordering::Relaxed) || context.locked.load(Ordering::Relaxed) {
        return;
    }
    let result = if context.property == PROPERTY_POWER {
        let on = lv_obj_has_state(target, lv_state_t_LV_STATE_CHECKED);
        post_property(
            context.entity,
            PROPERTY_POWER,
            if on { ACTION_ON } else { ACTION_OFF },
            CommandValue::None,
        )
    } else {
        let value = lv_slider_get_value(target) as f32;
        post_property(
            context.entity,
            context.property,
            ACTION_SET,
            CommandValue::Scalar(Value::f32(value)),
        )
    };
    if let Err(error) = result {
        log::warn!("display: command rejected: {error:?}");
    }
    if context.property == PROPERTY_POWER {
        context.locked.store(true, Ordering::Relaxed);
        *context.deadline.lock().unwrap() =
            Some(Instant::now() + Duration::from_millis(SWITCH_LOCK_MS));
        lv_obj_add_state(target, lv_state_t_LV_STATE_DISABLED);
    }
}

unsafe extern "C" fn free_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut CmdCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

unsafe extern "C" fn on_color(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *const ColorCtx;
    if context.is_null() {
        return;
    }
    let context = &*context;
    let hue = lv_slider_get_value(context.hue) as f32;
    let sat = lv_slider_get_value(context.sat) as f32;
    let (r, g, b) = hsv_to_rgb8(hue, sat / 100.0, 1.0);
    lv_obj_set_style_bg_color(context.swatch, color(rgb_hex(r, g, b)), 0);
    set_label(context.hue_label, &format!("{}°", hue.round() as i32));
    set_label(context.sat_label, &format!("{} %", sat.round() as i32));
    if lv_event_get_code(event) != lv_event_code_t_LV_EVENT_RELEASED {
        return;
    }
    let (x, y) = rgb8_to_xy(r, g, b);
    if let Err(error) = post_property(
        context.entity,
        PROPERTY_COLOR,
        ACTION_SET,
        CommandValue::Xy { x, y },
    ) {
        log::warn!("display: color rejected: {error:?}");
    }
}

unsafe extern "C" fn free_color_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut ColorCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

unsafe extern "C" fn on_automation(event: *mut lv_event_t) {
    let target = lv_event_get_target_obj(event);
    let context = lv_event_get_user_data(event) as *const AutoCtx;
    if target.is_null() || context.is_null() {
        return;
    }
    let on = lv_obj_has_state(target, lv_state_t_LV_STATE_CHECKED);
    if let Err(error) = post_automation((*context).automation, on) {
        log::warn!("display: automation toggle rejected: {error:?}");
    }
}

unsafe extern "C" fn free_auto_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut AutoCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}
